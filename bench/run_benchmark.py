#!/usr/bin/env python3
"""End-to-end Linux TCP benchmark, using inotify instead of timed file polling."""

import argparse
import csv
import ctypes
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time
from statistics import median

FRAME = struct.Struct(">IHHIIQd")
MAGIC = 0x544C5259  # TLRY
WATCH_EVENTS = 0x00000002 | 0x00000100 | 0x00000008  # MODIFY, CREATE, CLOSE_WRITE
ROOT = Path(__file__).resolve().parent.parent


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return number


def batches(value):
    try:
        values = [positive(item) for item in value.split(",")]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected comma-separated positive integers") from exc
    if not values or any(v > 4096 for v in values):
        raise argparse.ArgumentTypeError("batch size must be 1..4096")
    return list(dict.fromkeys(values))


def verify_file(path, frames):
    """Check the exact protocol payload, including every timestamp and its order."""
    if path.stat().st_size != frames * FRAME.size:
        raise RuntimeError(f"wrong file size: {path.stat().st_size}, expected {frames * FRAME.size}")
    expected_timestamp = 1
    with path.open("rb") as stream:
        while chunk := stream.read(FRAME.size * 8192):
            if len(chunk) % FRAME.size:
                raise RuntimeError("incomplete telemetry frame")
            for record in FRAME.iter_unpack(chunk):
                magic, version, size, sensor, metric, timestamp, value = record
                if (magic, version, size, sensor, metric, timestamp, value) != (
                    MAGIC, 1, FRAME.size, 1, 2, expected_timestamp, 1.0
                ):
                    raise RuntimeError(f"invalid or out-of-order frame #{expected_timestamp}: {record}")
                expected_timestamp += 1
    if expected_timestamp != frames + 1:
        raise RuntimeError("incorrect number of frames")


def watch_directory(path):
    libc = ctypes.CDLL(None, use_errno=True)
    libc.inotify_init1.argtypes = [ctypes.c_int]
    libc.inotify_init1.restype = ctypes.c_int
    libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
    libc.inotify_add_watch.restype = ctypes.c_int
    fd = libc.inotify_init1(os.O_CLOEXEC | os.O_NONBLOCK)
    if fd == -1:
        raise OSError(ctypes.get_errno(), "inotify_init1 failed")
    if libc.inotify_add_watch(fd, os.fsencode(path), WATCH_EVENTS) == -1:
        error = ctypes.get_errno()
        os.close(fd)
        raise OSError(error, "inotify_add_watch failed")
    return fd


def await_file_size(path, size, watcher, server, timeout=120):
    deadline = time.monotonic() + timeout
    while True:
        actual = path.stat().st_size if path.exists() else 0
        if actual == size:
            return
        if actual > size:
            raise RuntimeError(f"file too large: {actual} > {size} bytes")
        if server.poll() is not None:
            raise RuntimeError(f"server exited before writing all frames ({actual}/{size} bytes)")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"frames not fully written: expected={size}, actual={actual} bytes")
        # inotify notifications may not be forwarded reliably by host-mounted
        # filesystems (e.g. Docker Desktop VirtioFS). Poll the actual file
        # size at most every 100 ms even without an event. When inotify works,
        # it still wakes this loop immediately.
        readable, _, _ = select.select([watcher], [], [], min(remaining, 0.1))
        if readable:
            try:
                os.read(watcher, 65536)
            except BlockingIOError:
                pass


def await_exit(proc, timeout=20):
    # pidfd becomes readable as soon as Linux reports process termination.
    if hasattr(os, "pidfd_open"):
        fd = os.pidfd_open(proc.pid)
        try:
            if not select.select([fd], [], [], timeout)[0]:
                raise TimeoutError("server shutdown timed out")
        finally:
            os.close(fd)
        return proc.wait()
    return proc.wait(timeout=timeout)


def sender_metrics(output):
    values = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
    return float(values["sender_seconds"]), float(values["sender_frames_per_second"])


def run_once(frames, batch, port, server_binary, sender_binary):
    with tempfile.TemporaryDirectory(prefix="telemetry-benchmark-") as work:
        folder = Path(work)
        data = folder / "telemetry.bin"
        log_path = folder / "server.log"
        watcher = watch_directory(folder)
        proc = None
        successful = False
        try:
            with log_path.open("wb") as log:
                proc = subprocess.Popen(
                    [str(server_binary), "--port", str(port), "--data-file", str(data), "--quiet"],
                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True
                )
                # Startup happens before the timed interval.
                deadline = time.monotonic() + 10
                while True:
                    logs = log_path.read_text(errors="replace")
                    if "[server] listening on" in logs and "[reader] reading telemetry from shared memory" in logs:
                        break
                    if proc.poll() is not None:
                        raise RuntimeError("server exited before becoming ready")
                    if time.monotonic() >= deadline:
                        raise TimeoutError("server startup timed out")
                    time.sleep(0.01)

                start = time.monotonic_ns()
                sender = subprocess.run(
                    [str(sender_binary), "--frames", str(frames), "--batch-frames", str(batch), "--port", str(port)],
                    capture_output=True, text=True, timeout=90, check=True
                )
                sender_seconds, sender_rate = sender_metrics(sender.stdout)
                await_file_size(data, frames * FRAME.size, watcher, proc)
                proc.send_signal(signal.SIGTERM)
                if await_exit(proc) != 0:
                    raise RuntimeError(f"server exited with status {proc.returncode}")
                seconds = (time.monotonic_ns() - start) / 1_000_000_000

            if "[reader] EOF" not in log_path.read_text(errors="replace"):
                raise RuntimeError("Reader did not report a graceful EOF")
            # Verification is deliberately OUTSIDE the timed measurement.
            verify_file(data, frames)
            successful = True
            return {
                "frames": frames, "batch_frames": batch, "bytes": frames * FRAME.size,
                "sender_seconds": sender_seconds, "sender_frames_per_second": sender_rate,
                "end_to_end_seconds": seconds,
                "end_to_end_frames_per_second": frames / seconds,
                "verified_frames": frames,
            }
        except Exception:
            print("Server log (last 60 lines):", file=sys.stderr)
            if log_path.exists():
                print("\n".join(log_path.read_text(errors="replace").splitlines()[-60:]), file=sys.stderr)
            raise
        finally:
            os.close(watcher)
            if proc is not None:
                if not successful:
                    # The Reader can outlive an unexpectedly exited Server.
                    # A dedicated session makes it possible to stop both.
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                proc.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frames", nargs="?", type=positive, default=100000)
    parser.add_argument("batch", nargs="?", type=positive, default=128)
    parser.add_argument("port", nargs="?", type=positive, default=19000)
    parser.add_argument("--batches", type=batches, help="comma-separated batch sizes, e.g. 1,32,128,512")
    parser.add_argument("--repeat", type=positive, default=1, help="runs per batch size")
    parser.add_argument("--csv", type=Path, default=ROOT / "build-bench" / "benchmark_results.csv")
    args = parser.parse_args()
    sizes = args.batches if args.batches is not None else [args.batch]
    if args.frames > 1000000 or args.port > 65535 or args.repeat > 20 or any(x > 4096 for x in sizes):
        parser.error("limits: frames <= 1000000, port <= 65535, repeat <= 20, batch <= 4096")

    server = Path(os.environ.get("SERVER_BIN", ROOT / "build-bench" / "telemetry-server"))
    sender = Path(os.environ.get("SENDER_BIN", ROOT / "build-bench" / "telemetry-load"))
    if not server.is_file() or not sender.is_file():
        parser.error("missing executables: first run 'make benchmark' to build them")

    args.csv.parent.mkdir(parents=True, exist_ok=True)
    columns = ["batch_frames", "run", "frames", "bytes", "sender_seconds",
               "sender_frames_per_second", "end_to_end_seconds", "end_to_end_frames_per_second", "verified_frames"]
    with args.csv.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        for batch in sizes:
            rates = []
            durations = []
            for attempt in range(1, args.repeat + 1):
                print(f"\nBatch {batch}, run {attempt}/{args.repeat}", flush=True)
                result = run_once(args.frames, batch, args.port, server, sender)
                writer.writerow({"batch_frames": batch, "run": attempt, **result})
                output.flush()
                rates.append(result["end_to_end_frames_per_second"])
                durations.append(result["end_to_end_seconds"])
                print(f"sender_seconds={result['sender_seconds']:.3f}")
                print(f"sender_frames_per_second={result['sender_frames_per_second']:.0f}")
                print(f"persisted_frames={result['verified_frames']}")
                print(f"persisted_bytes={result['bytes']}")
                print(f"end_to_end_seconds={result['end_to_end_seconds']:.3f}")
                print(f"end_to_end_frames_per_second={result['end_to_end_frames_per_second']:.0f}")
                print("validated: complete ordered frame contents and graceful Reader EOF")
            print(f"SUMMARY batch={batch} runs={len(rates)} "
                  f"median_frames_per_second={median(rates):.0f} "
                  f"min={min(rates):.0f} max={max(rates):.0f} "
                  f"median_seconds={median(durations):.4f}")
    print(f"CSV: {args.csv}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError, ValueError) as exc:
        print(f"benchmark failed: {exc}", file=sys.stderr)
        sys.exit(1)
