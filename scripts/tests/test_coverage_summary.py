# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for coverage_summary's first-party line accounting."""
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

source = Path(__file__).resolve().parents[1] / "coverage_summary.py"
spec = importlib.util.spec_from_file_location("coverage_summary", source)
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


def sample(*files):
    """Construct a small Cobertura fixture for unit testing."""
    return ET.fromstring("<coverage><packages><package><classes>"
                         + "".join(files) + "</classes></package></packages></coverage>")


def cls(name, *lines):
    """Build a Cobertura class with per-line execution hit counts."""
    return '<class filename="' + name + '"><lines>' + "".join(
        '<line number="' + str(n) + '" hits="' + str(h) + '"/>' for n, h in lines
    ) + "</lines></class>"


class CoverageSummaryTests(unittest.TestCase):
    def test_unique_lines_across_duplicate_instantiations(self):
        """Repeated compilation of one source line is counted once."""
        report = coverage.read_coverage(sample(
            cls("compositor/src/render/frame.c", (2, 0), (3, 2)),
            cls("compositor/src/render/frame.c", (2, 4), (3, 0)),
            cls("shell/src/main.cpp", (10, 0)),
        ))
        self.assertEqual(report["total"]["tracked"], 3)
        self.assertEqual(report["total"]["covered"], 2)
        self.assertEqual(report["subsystems"]["compositor"]["percent"], 100)
        self.assertIn("shell", coverage.to_markdown(report))

    def test_generated_and_test_paths_not_allowed(self):
        """Never count generated, test, or parent-traversal source paths."""
        for path in ("tests/audio/config.cpp", "shell/tests/foo.cpp",
                     "build-coverage/qt_autogen.cpp",
                     "vela-gpu-build/qt_autogen.cpp", "../escape.cpp"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                coverage.read_coverage(sample(cls(path, (1, 1))))

    def test_empty_report_not_accepted(self):
        """Reject empty reports rather than uploading misleading percentages."""
        with self.assertRaises(ValueError):
            coverage.read_coverage(sample(cls("shell/src/main.cpp")))

    def test_negative_hits_rejected(self):
        """Negative GCC execution counts cannot enter coverage statistics."""
        with self.assertRaises(ValueError):
            coverage.read_coverage(sample(cls("shell/src/main.cpp", (3, -1))))


    def test_cli_reads_and_writes_only_fixed_local_report_names(self):
        """The CLI cannot take traversal paths; outputs stay in its report directory."""
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            xml = '<coverage><packages><package><classes>' + cls(
                "shell/src/main.cpp", (1, 1), (2, 0)
            ) + "</classes></package></packages></coverage>"
            (directory / "coverage.xml").write_text(xml, encoding="utf-8")
            proc = subprocess.run([sys.executable, str(source)], cwd=tmp, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            summary = json.loads((directory / "coverage-summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["total"]["covered"], 1)
            self.assertEqual(summary["total"]["tracked"], 2)
            self.assertTrue((directory / "coverage-summary.md").is_file())
            bad = subprocess.run([sys.executable, str(source), "../outside.md"], cwd=tmp, capture_output=True)
            self.assertNotEqual(bad.returncode, 0)


if __name__ == "__main__":
    unittest.main()
