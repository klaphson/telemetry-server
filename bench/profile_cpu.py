#!/usr/bin/env python3
"""Profile optimized telemetry-server CPU hotspots with Linux perf.

Runs the verified loopback benchmark repeatedly under perf so short test
durations still yield a useful sample set. This does not measure throughput
without profiling overhead.
"""

import argparse
import csv
import os
from pathlib import Path
import shutil
import subprocess
import sys

from run_benchmark import ROOT, positive


PERMISSION_CLUES = (
    "permission denied",
    "operation not permitted",
    "no permission",
    "perf_event_open",
    "perf_event_paranoid",
    "access to performance monitoring",
)


def record_command(perf, python, benchmark, perf_data, benchmark_args):
    # cpu-clock:u is a software sampling event restricted to user-space
    # instructions. Samples are inherited by child processes by default.
    return [
        perf, "record", "-e", "cpu-clock:u", "-F", "499",
        "--call-graph", "fp", "-o", str(perf_data), "--",
        python, str(benchmark), *benchmark_args,
    ]


def stat_command(perf, python, benchmark, stats_file, benchmark_args):
    return [
        perf, "stat", "-e", "task-clock,context-switches,page-faults",
        "-o", str(stats_file), "--",
        python, str(benchmark), *benchmark_args,
    ]


def benchmark_command_args(frames, batch, port, repeat, csv_file):
    return [
        str(frames), str(batch), str(port), "--repeat", str(repeat),
        "--csv", str(csv_file),
    ]


def validate_results(path, frames, batch, repeat):
    """Reject partial or failed benchmark runs, even if perf exits cleanly."""
    with Path(path).open(newline="") as csv_file:
        rows = list(csv.DictReader(csv_file))
    if len(rows) != repeat:
        raise RuntimeError(f"benchmark ran {len(rows)}/{repeat} repeats")
    for i, row in enumerate(rows, 1):
        if (int(row["run"]) != i or int(row["frames"]) != frames
                or int(row["batch_frames"]) != batch
                or int(row["verified_frames"]) != frames
                or int(row["bytes"]) != frames * 32):
            raise RuntimeError(f"invalid benchmark verification row {i}")
    return rows


def perf_hint(message):
    if any(clue in message.lower() for clue in PERMISSION_CLUES):
        return (
            "Linux denied perf_event_open. Docker Desktop/virtualized kernels "
            "may restrict perf even when running as root. Check "
            "'cat /proc/sys/kernel/perf_event_paranoid', and try profiling "
            "on a native Linux host with supported perf access. Do not "
            "enable privileged Docker settings for the production server."
        )
    if "not found" in message.lower() or "kernel" in message.lower():
        return (
            "The perf binary may not match the running kernel. On native "
            "Ubuntu install an appropriate linux-tools package and check "
            "'uname -r' plus 'perf --version'. Docker Desktop uses a "
            "VM kernel that may not have matching Ubuntu tools."
        )
    return "Inspect the saved perf log. Sampling needs kernel perf support."


def tail(path, limit=35):
    if not Path(path).is_file():
        return "(no log)"
    return "\n".join(Path(path).read_text(errors="replace").splitlines()[-limit:])


def run_logged(command, log_path, timeout, env=None):
    with Path(log_path).open("w") as stream:
        try:
            proc = subprocess.run(
                command, stdout=stream, stderr=subprocess.STDOUT,
                env=env, timeout=timeout, check=False
            )
        except subprocess.TimeoutExpired as exc:
            raise TimeoutError(f"profiling timed out; log: {log_path}") from exc
    if proc.returncode != 0:
        message = tail(log_path)
        raise RuntimeError(
            f"profiling process failed (exit {proc.returncode}):\n"
            f"{message}\n\n{perf_hint(message)}"
        )


def check_perf(perf, output):
    """Check ability to open an actual sampling event, not only perf --version."""
    check_data = output / "check.data"
    check_log = output / "check.log"
    try:
        run_logged(
            [
                perf, "record", "-e", "cpu-clock:u", "-F", "99",
                "-o", str(check_data), "--", sys.executable, "-c",
                "sum(n*n for n in range(1000000))",
            ],
            check_log, timeout=15
        )
    finally:
        check_data.unlink(missing_ok=True)
    print("perf sampling permission check: OK", flush=True)


def run_profile(args):
    perf = shutil.which("perf")
    if perf is None:
        raise RuntimeError(
            "perf was not found. On native Ubuntu, install the matching "
            "linux-tools package; Docker Desktop may not provide a "
            "usable perf binary for its VM kernel."
        )

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    check_perf(perf, output)
    if args.check:
        return

    server = Path(os.environ.get("SERVER_BIN", ROOT / "build-perf" / "telemetry-server"))
    sender = Path(os.environ.get("SENDER_BIN", ROOT / "build-perf" / "telemetry-load"))
    if not server.is_file() or not sender.is_file():
        raise RuntimeError("profile binaries not built; run 'make profile-cpu-build'")

    env = os.environ.copy()
    env["SERVER_BIN"] = str(server.resolve())
    env["SENDER_BIN"] = str(sender.resolve())
    bench = ROOT / "bench" / "run_benchmark.py"

    modes = ("record", "stat") if args.mode == "both" else (args.mode,)
    for mode in modes:
        bench_csv = output / f"{mode}_runs.csv"
        bench_csv.unlink(missing_ok=True)
        bench_args = benchmark_command_args(
            args.frames, args.batch, args.port, args.repeat, bench_csv
        )
        log_file = output / f"{mode}.log"
        if mode == "record":
            perf_data = output / "perf.data"
            perf_data.unlink(missing_ok=True)
            command = record_command(perf, sys.executable, bench, perf_data, bench_args)
            print(f"Recording samples (Server + Reader, {args.repeat} runs)...", flush=True)
        else:
            stat_file = output / "stat.txt"
            stat_file.unlink(missing_ok=True)
            command = stat_command(perf, sys.executable, bench, stat_file, bench_args)
            print(f"Counting CPU events ({args.repeat} runs)...", flush=True)

        run_logged(command, log_file, timeout=args.timeout, env=env)
        validate_results(bench_csv, args.frames, args.batch, args.repeat)

        if mode == "record":
            if not perf_data.is_file() or perf_data.stat().st_size == 0:
                raise RuntimeError("perf did not create a nonempty perf.data")
            report_path = output / "report.txt"
            report_command = [
                perf, "report", "--stdio", "-i", str(perf_data),
                "--no-children", "--sort", "pid,dso,symbol",
            ]
            run_logged(report_command, report_path, timeout=60)
            report_text = report_path.read_text(errors="replace")
            if "no samples" in report_text.lower() or len(report_text.strip()) < 50:
                raise RuntimeError("perf returned no useful CPU samples; increase --repeat")
            print(f"CPU report: {report_path}", flush=True)
            print("Preview (first 35 lines):")
            print("\n".join(report_text.splitlines()[:35]))
        else:
            print(f"CPU counters: {stat_file}", flush=True)
            print(tail(stat_file))

        print(f"Verified benchmark runs: {bench_csv}", flush=True)

    print(f"All outputs: {output}", flush=True)
    print(
        "NOTE: perf records the benchmark Python driver and sender as well as "
        "the Server and Reader. Compare telemetry-server PIDs/symbols in report.txt.",
        flush=True
    )
    print(
        "NOTE: sampling and debug flags change execution characteristics. "
        "Use make benchmark-matrix for unprofiled throughput.",
        flush=True
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", type=positive, default=1000000)
    parser.add_argument("--batch", type=positive, default=128)
    parser.add_argument("--port", type=positive, default=19000)
    parser.add_argument("--repeat", type=positive, default=15)
    parser.add_argument("--mode", choices=("record", "stat", "both"), default="record")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build-perf" / "cpu-profile")
    parser.add_argument("--timeout", type=positive, default=300)
    parser.add_argument("--check", action="store_true",
                        help="check perf sampling permissions only (no build needed)")
    args = parser.parse_args()
    if args.frames > 1000000 or args.batch > 4096 or args.port > 65535 or args.repeat > 20:
        parser.error("limits: frames <= 1000000, batch <= 4096, port <= 65535, repeat <= 20")
    run_profile(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, TimeoutError,
            subprocess.SubprocessError) as exc:
        print(f"CPU profiling failed: {exc}", file=sys.stderr)
        sys.exit(1)
