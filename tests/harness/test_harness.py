# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""CPU checks for test infrastructure: no compositor, Qt or GPU is required."""

import contextlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

from harness import Image, Session


class SessionCleanup(unittest.TestCase):
    def child(self, source):
        process = subprocess.Popen([sys.executable, "-c", source], start_new_session=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.reap, process)
        return process

    @staticmethod
    def reap(process):
        if process.poll() is None:
            process.kill()
        process.wait()
        process.stdout.close()
        process.stderr.close()

    def test_already_crashed_compositor_fails_the_scenario(self):
        session = Session()
        session.process = self.child("raise SystemExit(7)")
        session.process.wait(timeout=3)
        with self.assertRaisesRegex(AssertionError, "stopped with 7"):
            session.stop()

    def test_already_clean_compositor_exit_is_accepted(self):
        session = Session()
        session.process = self.child("raise SystemExit(0)")
        session.process.wait(timeout=3)
        session.stop()

    def test_deliberate_failure_must_match_the_explicit_expected_exit(self):
        for actual in (1, 7):
            with self.subTest(actual=actual):
                session = Session(expected_exit=1)
                session.process = self.child(f"raise SystemExit({actual})")
                session.process.wait(timeout=3)
                if actual == 1:
                    session.stop()
                else:
                    with self.assertRaisesRegex(AssertionError, "expected 1"):
                        session.stop()

    def test_running_compositor_is_stopped_and_reaped(self):
        session = Session()
        session.process = self.child("import signal,time\n"
                                     "signal.signal(signal.SIGTERM, lambda *_: exit(0))\n"
                                     "print('ready', flush=True)\nwhile True: time.sleep(1)")
        self.assertEqual(session.process.stdout.readline().strip(), "ready")
        session.stop()
        self.assertEqual(session.process.returncode, 0)

    def test_uncooperative_client_is_killed_and_reaped(self):
        session = Session()
        client = self.child("import signal,time\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                            "print('ready', flush=True)\nwhile True: time.sleep(1)")
        self.assertEqual(client.stdout.readline().strip(), "ready")
        session.clients.append(client)
        session.stop()
        self.assertIsNotNone(client.returncode)

    def test_failed_startup_removes_temporary_directory_and_closes_log(self):
        session = Session()
        with tempfile.TemporaryDirectory() as missing_build, patch("harness.BUILD", missing_build):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(FileNotFoundError):
                session.start()
        self.assertFalse(os.path.exists(session.directory))
        self.assertTrue(session.log.closed)


class ImageChecks(unittest.TestCase):
    @staticmethod
    def png(raw, width=2, height=6):
        def chunk(kind, data):
            return (struct.pack(">I", len(data)) + kind + data
                    + struct.pack(">I", zlib.crc32(kind + data)))
        header = struct.pack(">II5B", width, height, 8, 2, 0, 0, 0)
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

    def read(self, data):
        with tempfile.NamedTemporaryFile() as file:
            file.write(data)
            file.flush()
            return Image.read(file.name)

    def test_all_png_filters_against_known_pixels(self):
        # Fixed encoded scanlines, independently specified. The Paeth rows
        # exercise above, left, upper-left, tie breaking and byte wraparound.
        raw = bytes((0, 10, 20, 30, 40, 50, 60,
                     1, 15, 25, 35, 30, 30, 30,
                     2, 5, 5, 5, 5, 5, 5,
                     3, 15, 20, 25, 18, 18, 18,
                     4, 5, 5, 5, 221, 5, 216,
                     4, 20, 226, 40, 30, 20, 196))
        image = self.read(self.png(raw))
        self.assertEqual((image.width, image.height), (2, 6))
        self.assertEqual(image.rows, [bytes(row) for row in
                         ((10, 20, 30, 40, 50, 60), (15, 25, 35, 45, 55, 65),
                          (20, 30, 40, 50, 60, 70), (25, 35, 45, 55, 65, 75),
                          (30, 40, 50, 20, 70, 35), (50, 10, 90, 80, 60, 30))])

    def test_invalid_png_filter_and_scanline_length_are_rejected(self):
        for raw in (bytes((5, 1, 2, 3)), bytes((0, 1, 2))):
            with self.subTest(raw=raw), self.assertRaises(AssertionError):
                self.read(self.png(raw, width=1, height=1))

    def test_changed_bounds_include_all_rgb_channels_and_exclude_bottom_rows(self):
        original = Image(3, 3, [bytes(9)] * 3)
        changed = Image(3, 3, [bytes((0, 0, 1, 0, 0, 0, 0, 0, 0)),
                               bytes((0, 0, 0, 0, 1, 0, 0, 0, 0)),
                               bytes((0, 0, 0, 0, 0, 0, 1, 0, 0))])
        self.assertIsNone(original.changed_bounds(original))
        self.assertEqual(changed.changed_bounds(original), (0, 0, 3, 3))
        self.assertEqual(changed.changed_bounds(original, height=2), (0, 0, 2, 2))
        self.assertIsNone(changed.changed_bounds(original, height=0))
        with self.assertRaises(AssertionError):
            changed.changed_bounds(Image(1, 1, [bytes(3)]))


if __name__ == "__main__":
    unittest.main()
