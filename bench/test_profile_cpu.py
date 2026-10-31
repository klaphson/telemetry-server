"""Tests for the perf CPU profiling harness (do not require perf access)."""

import csv
import tempfile
import unittest
from pathlib import Path

from profile_cpu import (
    benchmark_command_args,
    perf_hint,
    record_command,
    stat_command,
    validate_results,
)


class PerfProfileTests(unittest.TestCase):
    def test_record_inherits_server_reader_and_keeps_pid_symbols(self):
        args = benchmark_command_args(1000, 128, 19000, 3, Path("/tmp/runs.csv"))
        command = record_command(
            "perf", "python3", Path("bench/run_benchmark.py"),
            Path("/tmp/perf.data"), args
        )
        self.assertEqual(command[:3], ["perf", "record", "-e"])
        self.assertIn("cpu-clock:u", command)
        self.assertIn("--call-graph", command)
        self.assertIn("fp", command)
        self.assertEqual(command[command.index("--") + 1:], [
            "python3", "bench/run_benchmark.py", *args
        ])

    def test_stat_uses_software_events(self):
        args = benchmark_command_args(100, 32, 19000, 2, Path("/tmp/stat.csv"))
        cmd = stat_command("perf", "python3", Path("benchmark.py"),
                           Path("/tmp/stat.txt"), args)
        self.assertEqual(cmd[0:2], ["perf", "stat"])
        self.assertIn("task-clock,context-switches,page-faults", cmd)
        self.assertEqual(cmd[cmd.index("--") + 1:], ["python3", "benchmark.py", *args])

    def test_validate_completed_benchmark_rows(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "runs.csv"
            columns = ["batch_frames", "run", "frames", "bytes", "verified_frames"]
            with path.open("w", newline="") as out:
                writer = csv.DictWriter(out, fieldnames=columns)
                writer.writeheader()
                for index in (1, 2, 3):
                    writer.writerow({"batch_frames": 128, "run": index,
                                     "frames": 100, "bytes": 3200,
                                     "verified_frames": 100})
            self.assertEqual(len(validate_results(path, 100, 128, 3)), 3)
            with self.assertRaisesRegex(RuntimeError, "2/4 repeats"):
                validate_results(path, 100, 128, 4)
            with self.assertRaisesRegex(RuntimeError, "verification row"):
                validate_results(path, 101, 128, 3)

    def test_permissions_diagnostics(self):
        self.assertIn("Docker", perf_hint("perf_event_open: Operation not permitted"))
        self.assertIn("uname -r", perf_hint("perf missing for current kernel"))


if __name__ == "__main__":
    unittest.main()
