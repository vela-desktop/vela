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
        directory = tempfile.TemporaryDirectory(prefix="vela-compare-test-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.outputs = set()

    def capture(self, command, **kwargs):
        target = Path(command[command.index("--capture-dir") + 1])
        self.outputs.add(target.parent)
        for scale in compare.SCALES:
            for size in compare.SIZES:
                for state in compare.STATES:
                    (target / f"{scale:g}-{size}-{state}.png").touch()
        return subprocess.CompletedProcess(command, 0, stdout="fixture renderer\n", stderr="")

    def run_comparison(self, *arguments):
        with (mock.patch.object(compare.tempfile, "tempdir", str(self.root)),
              mock.patch.object(compare.subprocess, "run", side_effect=self.capture),
              contextlib.redirect_stdout(io.StringIO())):
            self.assertEqual(compare.main(["fixture-renderer", *arguments]), 0)

    def test_defaults_are_unique_private_and_keep_reports(self):
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
        output = self.root / "build" / "text-rendering-frames" / "comparison"
        self.run_comparison("--output", str(output))
        self.assertEqual(self.outputs, {output.resolve()})
        self.assertTrue((output / "index.html").is_file())


if __name__ == "__main__":
    unittest.main()
