#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Guard build job structure and ownership of the updater contract suite."""

from collections import Counter
from pathlib import Path
import re
import shlex
import unittest


ROOT = Path(__file__).resolve().parents[2]


class CIUpdaterTestOwnership(unittest.TestCase):
    def test_build_job_names_are_unique(self):
        """Reject duplicated job keys instead of accepting a silently overwritten workflow."""
        jobs = (ROOT / ".github/workflows/build.yml").read_text().split("\njobs:\n", 1)[1]
        names = re.findall(r"^  ([\w-]+):\s*$", jobs, re.M)
        duplicates = [name for name, count in Counter(names).items() if count > 1]
        self.assertFalse(duplicates, f"Duplicate build jobs: {duplicates}")

    def test_fedora_44_and_45_are_both_release_gates(self):
        build = (ROOT / ".github/workflows/build.yml").read_text()
        fedora = (ROOT / ".github/workflows/fedora.yml").read_text()
        for workflow in (build, fedora):
            self.assertIn("fedora: 44", workflow)
            self.assertIn("fedora: 45", workflow)
        self.assertIn("artifact: vela-fedora-rpm", build)
        self.assertIn("artifact: vela-fedora-45-rpm", build)
        self.assertIn("SHA256SUMS-fedora-45", build)
        self.assertIn("fedora-package-smoke", build)
        self.assertIn("fedora-tests, fedora-package, fedora-package-smoke", build)

    def test_updater_suite_runs_once_in_shared_packaging_job(self):
        """Keep distro-oriented updater contracts in their single shared packaging job."""
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
        self.assertEqual(ctest_args[0], "ctest")
        self.assertIn("--test-dir", ctest_args)
        self.assertGreater(len(ctest_args), ctest_args.index("--test-dir") + 1)
        self.assertEqual(ctest_args[ctest_args.index("--test-dir") + 1], "build")
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
