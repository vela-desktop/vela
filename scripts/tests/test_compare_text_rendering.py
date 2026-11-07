#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The comparison's default output must not reuse a publicly predictable path."""

import contextlib
import importlib.util
import io
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "compare-text-rendering.py"
spec = importlib.util.spec_from_file_location("vela_compare_text_rendering", SCRIPT)
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class ComparisonOutputTests(unittest.TestCase):
    def setUp(self):
        """Give each case a private output root and record renderer destinations."""
        directory = tempfile.TemporaryDirectory(prefix="vela-compare-test-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.outputs = set()

    def capture(self, command, **kwargs):
        """Stand in for a successful renderer by creating every expected frame."""
        target = Path(command[command.index("--capture-dir") + 1])
        self.outputs.add(target.parent)
        for scale in compare.SCALES:
            for size in compare.SIZES:
                for state in compare.STATES:
                    (target / f"{scale:g}-{size}-{state}.png").touch()
        return subprocess.CompletedProcess(command, 0, stdout="fixture renderer\n", stderr="")

    def run_comparison(self, *arguments):
        """Generate a report with the fixture renderer in an isolated temporary root."""
        with (mock.patch.object(compare.tempfile, "tempdir", str(self.root)),
              mock.patch.object(compare.subprocess, "run", side_effect=self.capture),
              contextlib.redirect_stdout(io.StringIO())):
            self.assertEqual(compare.main(["fixture-renderer", *arguments]), 0)

    def test_defaults_are_unique_private_and_keep_reports(self):
        """Fresh private outputs must ignore a precreated legacy-path symlink."""
        victim = self.root / "existing-data"
        victim.mkdir()
        sentinel = victim / "index.html"
        sentinel.write_text("keep this report")
        (self.root / "vela-text-comparison").symlink_to(victim, target_is_directory=True)

        self.run_comparison()
        self.run_comparison()

        self.assertEqual(len(self.outputs), 2)
        for output in self.outputs:
            self.assertEqual(output.parent, self.root.resolve())
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o700)
            self.assertTrue((output / "index.html").is_file())
        self.assertEqual(sentinel.read_text(), "keep this report")
        self.assertEqual(list(victim.iterdir()), [sentinel])

    def test_explicit_ci_output_is_preserved(self):
        """An explicit build artifact destination must contain the resulting report."""
        output = self.root / "build" / "text-rendering-frames" / "comparison"
        self.run_comparison("--output", str(output))
        self.assertEqual(self.outputs, {output.resolve()})
        self.assertTrue((output / "index.html").is_file())

    def test_failed_and_incomplete_captures_leave_no_report(self):
        """Unavailable, failed and incomplete renders must invalidate any old report."""
        for status in (77, 5, 0):
            for old_report in (False, True):
                with self.subTest(status=status, old_report=old_report):
                    output = self.root / f"failed-{status}-{old_report}"
                    output.mkdir()
                    report = output / "index.html"
                    if old_report:
                        report.write_text("old successful report")
                    result = subprocess.CompletedProcess([], status, stdout="", stderr="fixture failure")
                    with (mock.patch.object(compare.subprocess, "run", return_value=result),
                          contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error):
                        compare.main(["fixture-renderer", "--output", str(output)])
                    self.assertEqual(error.exception.code, status or 1)
                    self.assertFalse(report.exists())

    def test_reused_output_cannot_supply_stale_frames(self):
        """A successful exit without new frames must fail even after a complete run."""
        output = self.root / "reused"
        self.run_comparison("--output", str(output))
        note = output / "native" / "notes.txt"
        note.write_text("keep unrelated files")
        result = subprocess.CompletedProcess([], 0, stdout="", stderr="")
        with (mock.patch.object(compare.subprocess, "run", return_value=result),
              contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error):
            compare.main(["fixture-renderer", "--output", str(output)])
        self.assertEqual(error.exception.code, 1)
        self.assertFalse((output / "index.html").exists())
        self.assertFalse(list((output / "native").glob("*.png")))
        self.assertEqual(note.read_text(), "keep unrelated files")


if __name__ == "__main__":
    unittest.main()
