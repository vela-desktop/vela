# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for coverage_summary's first-party line accounting."""
import importlib.util
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

source = Path(__file__).resolve().parents[1] / "coverage_summary.py"
spec = importlib.util.spec_from_file_location("coverage_summary", source)
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


def sample(*files):
    return ET.fromstring("<coverage><packages><package><classes>"
                         + "".join(files) + "</classes></package></packages></coverage>")


def cls(name, *lines):
    return '<class filename="' + name + '"><lines>' + "".join(
        '<line number="' + str(n) + '" hits="' + str(h) + '"/>' for n, h in lines
    ) + "</lines></class>"


class CoverageSummaryTests(unittest.TestCase):
    def test_unique_lines_across_duplicate_instantiations(self):
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
        for path in ("tests/audio/config.cpp", "shell/tests/foo.cpp",
                     "build-coverage/qt_autogen.cpp",
                     "vela-gpu-build/qt_autogen.cpp", "../escape.cpp"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                coverage.read_coverage(sample(cls(path, (1, 1))))

    def test_empty_report_not_accepted(self):
        with self.assertRaises(ValueError):
            coverage.read_coverage(sample(cls("shell/src/main.cpp")))

    def test_negative_hits_rejected(self):
        with self.assertRaises(ValueError):
            coverage.read_coverage(sample(cls("shell/src/main.cpp", (3, -1))))


if __name__ == "__main__":
    unittest.main()
