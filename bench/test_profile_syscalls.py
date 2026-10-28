"""Tests for per-process strace parsing (no strace installation required)."""

import tempfile
import unittest
from pathlib import Path

from profile_syscalls import identify_processes, parse_trace, tracer_pid_from_status


class StraceProfileTests(unittest.TestCase):
    def test_counts_completed_and_resumed_calls_only(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.123"
            path.write_text(
                'write(9</tmp/telemetry.bin>, ""..., 32) = 32 <0.000020>\n'
                'read(8<anon_inode:[eventfd]>, ""..., 8 <unfinished ...>\n'
                '<... read resumed>...) = 8 <0.002000>\n'
                'read(8<anon_inode:[eventfd]>, ""..., 8) = -1 EAGAIN (Try again) <0.000005>\n'
                'fdatasync(9</tmp/telemetry.bin>) = 0 <0.000100>\n'
                '+++ exited with 0 +++\n'
            )
            stats = parse_trace(path)
            self.assertEqual(stats["write"][:2], [1, 0])
            self.assertEqual(stats["read"][:2], [2, 1])
            self.assertEqual(stats["fdatasync"][:2], [1, 0])
            self.assertAlmostEqual(stats["read"][2], 0.002005)

    def test_extracts_both_real_process_pids(self):
        log = "[server] pid=123\n[reader] pid=124\n[server] listening on 0.0.0.0:19000\n"
        self.assertEqual(identify_processes(log), (123, 124))
        self.assertIsNone(identify_processes("[server] pid=123\n"))

    def test_tracer_pid_parsing(self):
        self.assertEqual(tracer_pid_from_status("Name:\ttelemetry\nTracerPid:\t1234\n"), 1234)
        self.assertEqual(tracer_pid_from_status("Name:\ttelemetry\nTracerPid:\t0\n"), 0)
        self.assertEqual(tracer_pid_from_status("Name:\ttelemetry\n"), 0)

    def test_ignores_unfinished_calls_without_completed_resumes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.1"
            path.write_text(
                'epoll_wait(5, <unfinished ...>\n'
                '--- SIGTERM {si_signo=SIGTERM} ---\n'
                '+++ killed by SIGKILL +++\n'
            )
            self.assertEqual(parse_trace(path), {})


if __name__ == "__main__":
    unittest.main()
