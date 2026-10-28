"""Unit tests for correctness checks in the performance harness."""

import importlib.util
import os
import struct
import tempfile
import threading
import time
import unittest
from pathlib import Path

MODULE = Path(__file__).with_name("run_benchmark.py")
spec = importlib.util.spec_from_file_location("telemetry_benchmark", MODULE)
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class BenchmarkChecks(unittest.TestCase):
    def write_frames(self, path, count):
        with path.open("wb") as out:
            for index in range(1, count + 1):
                out.write(struct.pack(">IHHIIQd", bench.MAGIC, 1, 32, 1, 2, index, 1.0))

    def test_accepts_exact_frames(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "data.bin"
            self.write_frames(file, 100)
            bench.verify_file(file, 100)

    def test_rejects_reordered_frames_with_the_same_file_size(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "data.bin"
            self.write_frames(file, 2)
            data = file.read_bytes()
            file.write_bytes(data[32:] + data[:32])
            with self.assertRaisesRegex(RuntimeError, "out-of-order frame #1"):
                bench.verify_file(file, 2)

    def test_rejects_mutated_value_with_the_same_file_size(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "data.bin"
            self.write_frames(file, 1)
            data = bytearray(file.read_bytes())
            data[-1] ^= 1
            file.write_bytes(data)
            with self.assertRaisesRegex(RuntimeError, "invalid or out-of-order"):
                bench.verify_file(file, 1)

    def test_rejects_missing_records(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "data.bin"
            self.write_frames(file, 1)
            with self.assertRaisesRegex(RuntimeError, "wrong file size"):
                bench.verify_file(file, 2)

    def test_file_size_wait_falls_back_when_inotify_is_silent(self):
        # Simulate a host-mounted filesystem that changes a file but never
        # delivers an inotify event. The watcher is an unreadable pipe.
        class RunningServer:
            def poll(self):
                return None

        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "telemetry.bin"
            reader, writer = os.pipe()
            try:
                def save_frame():
                    time.sleep(0.05)
                    file.write_bytes(b"x" * 32)

                thread = threading.Thread(target=save_frame)
                thread.start()
                try:
                    bench.await_file_size(file, 32, reader, RunningServer(), timeout=2)
                    self.assertEqual(file.stat().st_size, 32)
                finally:
                    thread.join(timeout=2)
                    self.assertFalse(thread.is_alive())
            finally:
                os.close(reader)
                os.close(writer)

    def test_batch_parser_rejects_over_limit(self):
        self.assertEqual(bench.batches("1,32,128,512,128"), [1, 32, 128, 512])
        with self.assertRaises(Exception):
            bench.batches("1,4097")


if __name__ == "__main__":
    unittest.main()
