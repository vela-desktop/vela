#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Guard CI's distro-specific ownership of the updater contract suite."""

from pathlib import Path
import shlex
import unittest


ROOT = Path(__file__).resolve().parents[2]


class CIUpdaterTestOwnership(unittest.TestCase):
    def test_updater_suite_runs_once_in_shared_packaging_job(self):
        build = (ROOT / ".github/workflows/build.yml").read_text()
        fedora = (ROOT / ".github/workflows/fedora.yml").read_text()

        # The packaging job runs on PRs and main pushes, independently of CTest.
        self.assertIn("  pull_request:", build)
        self.assertIn("  push:", build)
        self.assertIn("python3 -m unittest discover -s packaging/tests -v", build)
        # Check the CTest arguments, not their literal formatting/order:
        # CI may add parallelism or other flags without rerunning updater tests.
        ctest_runs = [
            shlex.split(line.strip().removeprefix("run:").strip())
            for line in build.splitlines()
            if line.strip().startswith("run: ctest ")
        ]
        self.assertEqual(len(ctest_runs), 1)
        ctest_args = ctest_runs[0]
        self.assertEqual(ctest_args[:3], ["ctest", "--test-dir", "build"])
        self.assertIn("--output-on-failure", ctest_args)
        self.assertIn("-E", ctest_args)
        self.assertGreater(len(ctest_args), ctest_args.index("-E") + 1)
        self.assertEqual(ctest_args[ctest_args.index("-E") + 1], "^packaging-update$")

        # Fedora cannot re-run Arch-oriented updater mocks just because the
        # event changes from pull_request to push/dispatch.
        self.assertIn(
            "-LE gpu -E '^packaging-update$'",
            fedora,
        )
        self.assertEqual(fedora.count("ctest --test-dir build-fedora"), 1)
        self.assertNotIn("GITHUB_EVENT_NAME", fedora)


if __name__ == "__main__":
    unittest.main()
