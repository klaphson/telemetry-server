#!/usr/bin/env python3
"""Profile Server and Reader syscalls separately with strace -ff -T.

Linux only. The traced run is for syscall counts, not throughput comparisons.
"""

import argparse
from collections import defaultdict
import csv
import os
from pathlib import Path
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from run_benchmark import (
    FRAME,
    ROOT,
    await_file_size,
    verify_file,
    watch_directory,
)

# Limit output size: tracing every syscall for 500,000 frames is very expensive.
SYSCALLS = (
    "read,write,writev,pwrite64,recvfrom,recvmsg,sendto,sendmsg,"
    "epoll_wait,epoll_pwait,epoll_pwait2,fdatasync,fsync,accept4,eventfd2"
)
STARTED_SERVER = re.compile(r"\[server\] pid=(\d+)")
STARTED_READER = re.compile(r"\[reader\] pid=(\d+)")
STARTED_LISTENER = "[server] listening on"
STARTED_READER_READY = "[reader] reading telemetry from shared memory"
ORDINARY_CALL = re.compile(r"^([a-zA-Z_][a-zA-Z_0-9]*)\(")
RESUMED_CALL = re.compile(r"^<\.\.\. ([a-zA-Z_][a-zA-Z_0-9]*) resumed>")
DURATION = re.compile(r"\s<([0-9]+(?:\.[0-9]+)?)>$")
ERROR = re.compile(r"\s+=\s+-1(?:\s|$)")


def parse_trace(path):
    """Count completed calls, errors and observed wall durations from -T."""
    counts = defaultdict(lambda: [0, 0, 0.0])
    with Path(path).open(encoding="utf-8", errors="replace") as trace:
        for line in trace:
            match = ORDINARY_CALL.match(line) or RESUMED_CALL.match(line)
            duration = DURATION.search(line.rstrip())
            if not match or not duration:
                # An unfinished syscall gets its count on the 'resumed' line.
                continue
            count = counts[match.group(1)]
            count[0] += 1
            count[1] += bool(ERROR.search(line))
            count[2] += float(duration.group(1))
    return dict(counts)


def identify_processes(log):
    """Return the PID pair printed by the real executable."""
    server = STARTED_SERVER.search(log)
    reader = STARTED_READER.search(log)
    if not server or not reader:
        return None
    return int(server.group(1)), int(reader.group(1))


def wait_until_ready(log_path, server, timeout=30):
    """Wait for startup WITHOUT strace: the application's fork is not traced."""
    deadline = time.monotonic() + timeout
    while True:
        log = log_path.read_text(errors="replace")
        pids = identify_processes(log)
        if pids and STARTED_LISTENER in log and STARTED_READER_READY in log:
            if pids[0] != server.pid:
                raise RuntimeError("Server PID in log does not match started process")
            return pids
        if server.poll() is not None:
            raise RuntimeError("server exited during startup; inspect server.log")
        if time.monotonic() >= deadline:
            raise TimeoutError("server startup timed out")
        time.sleep(0.01)


def tracer_pid_from_status(status):
    for line in status.splitlines():
        if line.startswith("TracerPid:"):
            return int(line.split(":", 1)[1].strip())
    return 0


def read_tracer_pid(pid):
    try:
        return tracer_pid_from_status(Path(f"/proc/{pid}/status").read_text())
    except FileNotFoundError:
        return 0


def wait_until_traced(pids, tracer, trace_log_path, timeout=10):
    """Do not start the sender until strace has attached to BOTH processes."""
    deadline = time.monotonic() + timeout
    while True:
        owners = [read_tracer_pid(pid) for pid in pids]
        if all(owner == tracer.pid for owner in owners):
            return
        if tracer.poll() is not None:
            errors = trace_log_path.read_text(errors="replace")
            raise RuntimeError(f"strace attach failed (exit={tracer.returncode}): {errors[-3000:]}")
        if time.monotonic() >= deadline:
            errors = trace_log_path.read_text(errors="replace")
            raise TimeoutError(f"strace did not attach to both PIDs {pids}: {errors[-3000:]}")
        time.sleep(0.01)


def tail(path, lines=15):
    if not path.exists():
        return "(file not created)"
    return "\n".join(path.read_text(errors="replace").splitlines()[-lines:])


def process_status(pid):
    try:
        status = Path(f"/proc/{pid}/status").read_text()
        return " ".join(line for line in status.splitlines()
                        if line.startswith(("State:", "PPid:", "TracerPid:")))
    except FileNotFoundError:
        return "process already exited"


def write_summary(output, pids):
    rows = []
    for role, pid in zip(("Server", "Reader"), pids):
        trace_path = output / f"trace.{pid}"
        if not trace_path.is_file():
            raise RuntimeError(f"missing strace output for {role} (PID {pid}): {trace_path}")
        counts = parse_trace(trace_path)
        print(f"\n{role} (PID {pid}) — traced syscalls")
        print(f"{'syscall':<19} {'calls':>12} {'errors':>9} {'elapsed_s':>13}")
        for name, (calls, errors, seconds) in sorted(
            counts.items(), key=lambda item: (-item[1][0], item[0])
        ):
            print(f"{name:<19} {calls:>12} {errors:>9} {seconds:>13.6f}")
            rows.append((role, pid, name, calls, errors, f"{seconds:.6f}"))
    csv_path = output / "syscalls.csv"
    with csv_path.open("w", newline="") as output_file:
        writer = csv.writer(output_file)
        writer.writerow(["process", "pid", "syscall", "calls", "errors", "elapsed_seconds"])
        writer.writerows(rows)
    return csv_path


def run(frames, batch, port, output, server_binary, sender_binary, strace_binary, timeout=45):
    output.mkdir(parents=True, exist_ok=True)
    # The trace logs stay under /workspace, but telemetry data must be
    # written on the container's native Linux filesystem. Host-mounted
    # VirtioFS may not forward inotify events back to the container.
    data_dir = Path(tempfile.mkdtemp(prefix="telemetry-strace-storage-"))
    for old in output.glob("trace.*"):
        if re.fullmatch(r"trace\.\d+", old.name):
            old.unlink()
    (output / "syscalls.csv").unlink(missing_ok=True)

    log_path = output / "server.log"
    trace_log_path = output / "strace.log"
    data_path = data_dir / "telemetry.bin"

    watcher = None
    server = None
    tracer = None
    pids = None
    ok = False
    try:
        watcher = watch_directory(data_dir)
        print(f"Native Linux temporary storage: {data_dir}", flush=True)
        with log_path.open("wb") as log, trace_log_path.open("wb") as trace_log:
            # Start both processes normally, without ptrace following fork().
            server = subprocess.Popen(
                [str(server_binary), "--port", str(port), "--data-file", str(data_path), "--quiet"],
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
            )
            pids = wait_until_ready(log_path, server)
            print(f"Server PID={pids[0]}, Reader PID={pids[1]}", flush=True)

            # Attach only after the Reader is running. This leaves the fork
            # and child startup untraced, and trace.<PID> stays separate.
            tracer = subprocess.Popen(
                [str(strace_binary), "-ff", "-T", "-s", "0", "-yy",
                 "-e", f"trace={SYSCALLS}", "-o", str(output / "trace"),
                 "-p", str(pids[0]), "-p", str(pids[1])],
                stdout=trace_log, stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            wait_until_traced(pids, tracer, trace_log_path)
            print("strace attached to BOTH processes", flush=True)
            print(f"Trace directory: {output}", flush=True)

            sender = subprocess.run(
                [str(sender_binary), "--frames", str(frames),
                 "--batch-frames", str(batch), "--port", str(port)],
                capture_output=True, text=True, check=True, timeout=90,
            )
            print(sender.stdout.strip(), flush=True)
            # Do not poll the directory containing strace's own output.
            await_file_size(data_path, frames * FRAME.size, watcher, server,
                            timeout=timeout)

            os.kill(pids[0], signal.SIGTERM)
            try:
                code = server.wait(timeout=30)
            except subprocess.TimeoutExpired as exc:
                raise TimeoutError("server did not finish after SIGTERM") from exc
            if code != 0:
                raise RuntimeError(f"server exited with status {code}")
            try:
                tracer.wait(timeout=15)
            except subprocess.TimeoutExpired as exc:
                raise TimeoutError("strace did not detach after server exit") from exc
            if tracer.returncode != 0:
                raise RuntimeError(f"strace exited with status {tracer.returncode}")

        if "[reader] EOF" not in log_path.read_text(errors="replace"):
            raise RuntimeError("Reader did not finish cleanly")
        verify_file(data_path, frames)
        print(f"Verified {frames} complete, ordered 32-byte frames")
        csv_path = write_summary(output, pids)
        print(f"\nTrace logs: {output}/trace.<PID>")
        print(f"CSV summary: {csv_path}")
        print("WARNING: -T includes blocking waits; elapsed_s is NOT CPU time.")
        print("WARNING: tracing changes scheduling and throughput. Do not benchmark speed here.")
        ok = True
    except Exception:
        print(f"Profile output retained in: {output}", file=sys.stderr)
        print(f"Server log (last 40 lines):\n{tail(log_path, 40)}", file=sys.stderr)
        print(f"strace messages:\n{tail(trace_log_path, 20)}", file=sys.stderr)
        actual = data_path.stat().st_size if data_path.exists() else 0
        print(f"Storage bytes: {actual}/{frames * FRAME.size}", file=sys.stderr)
        if pids is not None:
            for role, pid in zip(("Server", "Reader"), pids):
                print(f"{role} PID={pid}: {process_status(pid)}", file=sys.stderr)
                print(f"{role} trace (last 15 lines):\n"
                      f"{tail(output / f'trace.{pid}')}", file=sys.stderr)
        raise
    finally:
        if watcher is not None:
            os.close(watcher)
        if not ok:
            if server is not None:
                try:
                    os.killpg(server.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if tracer is not None:
                try:
                    os.killpg(tracer.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        if tracer is not None:
            tracer.wait()
        if server is not None:
            server.wait()
        # Only temporary telemetry bytes are deleted. Raw strace traces,
        # server.log, strace.log and any completed CSV stay in output.
        shutil.rmtree(data_dir)


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", type=positive, default=10000,
                        help="number of records (1..50000); default 10000")
    parser.add_argument("--batch", type=positive, default=128,
                        help="TCP send batch (1..4096); default 128")
    parser.add_argument("--port", type=positive, default=19000)
    parser.add_argument("--output", type=Path, default=ROOT / "build-bench" / "syscall-profile")
    parser.add_argument("--timeout", type=positive, default=45,
                        help="maximum seconds to wait for traced storage writes (default 45)")
    args = parser.parse_args()
    if args.frames > 50000 or args.batch > 4096 or args.port > 65535:
        parser.error("limits: frames <= 50000, batch <= 4096, port <= 65535")
    strace_binary = shutil.which("strace")
    if strace_binary is None:
        parser.error("strace not found; install it with 'apt-get install strace'")
    server = Path(os.environ.get("SERVER_BIN", ROOT / "build-bench" / "telemetry-server"))
    sender = Path(os.environ.get("SENDER_BIN", ROOT / "build-bench" / "telemetry-load"))
    if not server.is_file() or not sender.is_file():
        parser.error("build first: make benchmark-build")
    run(args.frames, args.batch, args.port, args.output.resolve(), server.resolve(),
        sender.resolve(), strace_binary, timeout=args.timeout)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as exc:
        print(f"syscall profiling failed: {exc}", file=sys.stderr)
        sys.exit(1)
