#!/usr/bin/env python3
"""Bounded multi-client TCP integration tests for both Reader execution modes.

Runs only in development/CTest. Uses a unique loopback port and temporary data.
Global record order across clients is intentionally unspecified; order within
any single TCP connection must be preserved exactly.
"""

import argparse
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

FRAME = struct.Struct(">IHHIIQd")
MAGIC = 0x544C5259


def encode(sensor, sequence):
    return FRAME.pack(MAGIC, 1, FRAME.size, sensor, 7, sequence, float(sensor))


def verify_records(path, counts):
    contents = path.read_bytes()
    expected_size = sum(counts.values()) * FRAME.size
    if len(contents) != expected_size:
        raise AssertionError(f"expected {expected_size} stored bytes, got {len(contents)}")
    positions = defaultdict(int)
    for offset in range(0, len(contents), FRAME.size):
        frame = contents[offset:offset + FRAME.size]
        magic, version, size, sensor, metric, sequence, value = FRAME.unpack(frame)
        if sensor not in counts:
            raise AssertionError(f"unexpected sensor {sensor} at byte {offset}")
        expected_sequence = positions[sensor] + 1
        if frame != encode(sensor, expected_sequence):
            raise AssertionError(
                f"corrupt, duplicate or out-of-order frame: sensor={sensor} "
                f"sequence={sequence} expected={expected_sequence} at byte={offset} "
                f"header=({magic},{version},{size},{metric},{value})"
            )
        positions[sensor] += 1
    for sensor, count in counts.items():
        if positions[sensor] != count:
            raise AssertionError(f"sensor {sensor}: expected {count}, got {positions[sensor]}")
    return sum(positions.values())


def self_test():
    with tempfile.TemporaryDirectory(prefix="telemetry-multi-unit-") as directory:
        path = Path(directory) / "data.bin"
        valid = encode(101, 1) + encode(102, 1) + encode(101, 2)
        path.write_bytes(valid)
        assert verify_records(path, {101: 2, 102: 1}) == 3
        for damaged in (valid[:-1], valid + valid[-32:],
                        encode(101, 2) + encode(102, 1) + encode(101, 1)):
            path.write_bytes(damaged)
            try:
                verify_records(path, {101: 2, 102: 1})
            except AssertionError:
                continue
            raise AssertionError("verifier accepted incorrect persisted data")
    print("multi-client frame verifier self-test: PASS")


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_ready(proc, log_path, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        log = log_path.read_text(errors="replace")
        if "[server] listening on" in log and "[reader] reading telemetry from shared memory" in log:
            return
        if proc.poll() is not None:
            raise AssertionError(f"server terminated during startup: {proc.returncode}")
        time.sleep(0.02)
    raise TimeoutError("server/reader startup timed out")


def send_client(port, sensor, count, barrier, behavior):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.settimeout(5)
        barrier.wait(timeout=5)
        if behavior == "idle":
            time.sleep(0.15)
        for start in range(1, count + 1, 64):
            frames = [encode(sensor, seq) for seq in range(start, min(count + 1, start + 64))]
            if behavior == "fragmented":
                for frame in frames:
                    sock.sendall(frame[:7])
                    time.sleep(0.0005)
                    sock.sendall(frame[7:19])
                    sock.sendall(frame[19:])
            elif behavior == "slow":
                for frame in frames:
                    sock.sendall(frame)
                    time.sleep(0.003)
            else:
                sock.sendall(b"".join(frames))
        if behavior == "partial":
            sock.sendall(encode(sensor, count + 1)[:17])
        elif behavior == "invalid":
            sock.sendall(bytes(FRAME.size))
        # The server may close an invalid connection after the valid prefix.
    return sensor, count


def send_hot(port, stop, ready, stats):
    sensor = 301
    count = 0
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.settimeout(5)
        while not stop.is_set():
            data = b"".join(encode(sensor, n) for n in range(count + 1, count + 129))
            sock.sendall(data)
            count += 128
            with stats["lock"]:
                stats["sent"] = count
            ready.set()
            # Sustained traffic, deliberately paced to avoid making the test
            # depend on a particular disk or scheduler throughput.
            time.sleep(0.0005)
    return count


def wait_hot_progress_and_probe(port, data_path, proc, timeout=8):
    probe = encode(302, 1)
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.settimeout(5)
        sock.sendall(probe[:11])
        time.sleep(0.002)
        sock.sendall(probe[11:])
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError("server exited while two clients were active")
        if data_path.exists() and probe in data_path.read_bytes():
            return
        time.sleep(0.02)
    raise TimeoutError("slow probe not persisted during sustained fast-client traffic")


def wait_size(data_path, expected_size, proc, timeout=12.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError("server exited before persisting all frames")
        if data_path.exists() and data_path.stat().st_size >= expected_size:
            return
        time.sleep(0.02)
    raise TimeoutError(f"expected {expected_size} data bytes, storage is incomplete")


def run_mode(binary, mode):
    with tempfile.TemporaryDirectory(prefix=f"telemetry-multi-{mode}-") as directory:
        folder = Path(directory)
        log_path = folder / "server.log"
        data_path = folder / "records.bin"
        port = unused_port()
        command = [str(binary), "--reader-mode", mode, "--port", str(port),
                   "--data-file", str(data_path), "--quiet"]
        proc = None
        success = False
        try:
            with log_path.open("wb") as log:
                proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
                wait_ready(proc, log_path)

                # Per-client FIFO is required; global cross-client ordering is not.
                clients = [(101, 1024, "fast"), (102, 1024, "fast"),
                           (103, 1024, "fast"), (201, 24, "slow"),
                           (202, 16, "fragmented"), (203, 32, "partial"),
                           (204, 8, "invalid"), (205, 3, "idle")]
                barrier = threading.Barrier(len(clients))
                with ThreadPoolExecutor(max_workers=len(clients)) as pool:
                    futures = [pool.submit(send_client, port, sensor, count, barrier, behavior)
                               for sensor, count, behavior in clients]
                    for future in futures:
                        future.result(timeout=12)
                expected = {sensor: count for sensor, count, _ in clients}

                # A malformed connection cannot take down unrelated clients.
                barrier = threading.Barrier(1)
                send_client(port, 206, 10, barrier, "fast")
                expected[206] = 10

                stop = threading.Event()
                ready = threading.Event()
                stats = {"lock": threading.Lock(), "sent": 0}
                with ThreadPoolExecutor(max_workers=1) as pool:
                    future = pool.submit(send_hot, port, stop, ready, stats)
                    try:
                        if not ready.wait(timeout=5):
                            raise TimeoutError("hot sender did not start")
                        wait_hot_progress_and_probe(port, data_path, proc)
                        with stats["lock"]:
                            progress_at_probe = stats["sent"]
                        deadline = time.monotonic() + 3
                        while time.monotonic() < deadline:
                            with stats["lock"]:
                                if stats["sent"] >= progress_at_probe + 256:
                                    break
                            time.sleep(0.01)
                        else:
                            raise TimeoutError("fast client stopped progressing after slow probe")
                    finally:
                        stop.set()
                    hot_count = future.result(timeout=10)
                expected[301] = hot_count
                expected[302] = 1

                wait_size(data_path, sum(expected.values()) * FRAME.size, proc)
                proc.send_signal(signal.SIGTERM)
                if proc.wait(timeout=12) != 0:
                    raise AssertionError(f"server returned nonzero exit status {proc.returncode}")
                log_text = log_path.read_text(errors="replace")
                if "[reader] EOF" not in log_text:
                    raise AssertionError("Reader did not log graceful EOF")
                if "protocol error" not in log_text:
                    raise AssertionError("malformed client was not rejected")
                verified = verify_records(data_path, expected)
                print(f"mode={mode} clients={len(expected)} frames={verified} "
                      f"hot_frames={hot_count} verified=PASS", flush=True)
                success = True
        except BaseException:
            print(f"Server log for {mode} (last 60 lines):", file=sys.stderr)
            if log_path.exists():
                print("\n".join(log_path.read_text(errors="replace").splitlines()[-60:]),
                      file=sys.stderr)
            raise
        finally:
            if proc is not None:
                if not success:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                proc.wait(timeout=12)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", nargs="?", type=Path,
                        help="absolute or relative telemetry-server path")
    parser.add_argument("--mode", choices=("process", "thread", "both"), default="both")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.server or not args.server.is_file():
        parser.error("provide the compiled telemetry-server executable")
    binary = args.server.resolve()
    for mode in (("process", "thread") if args.mode == "both" else (args.mode,)):
        run_mode(binary, mode)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, RuntimeError, TimeoutError, OSError,
            subprocess.SubprocessError) as error:
        print(f"multi-client integration test failed: {error}", file=sys.stderr)
        sys.exit(1)
