#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Guard build job structure and ownership of the updater contract suite."""

from collections import Counter
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import textwrap
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
        self.assertIn("updater_fixtures: true", build)
        for fixture in ("ci-updater-local-build.sh", "ci-dnf-transactions.sh"):
            with self.subTest(fixture=fixture):
                self.assertIn(fixture, fedora)
                self.assertNotIn(fixture, build)

    def test_package_builds_start_early_but_beta_waits_for_contracts(self):
        build = (ROOT / ".github/workflows/build.yml").read_text()
        for name in ("package", "fedora-package"):
            with self.subTest(job=name):
                self.assertRegex(build, rf"(?m)^  {name}:\n    needs: changes$")
        publish = build.split("\n  publish-beta:\n", 1)[1]
        dependencies = re.search(r"(?m)^    needs: \[([^\]]+)\]$", publish)
        self.assertIsNotNone(dependencies)
        self.assertIn("packaging-tests", dependencies[1].split(", "))

    def test_fedora_beta_tag_pipeline_is_version_independent(self):
        # 0.1.2 is built from release/0.1 and 0.2.0 from main. Neither
        # release number should be embedded in the RPM or release asset names.
        build = (ROOT / ".github/workflows/build.yml").read_text()
        spec = (ROOT / "packaging/fedora/vela.spec").read_text()
        self.assertIn("tags: ['v0.*', 'rehearsal/v0.*']", build)
        self.assertIn('version="$VELA_BETA_VERSION"', build)
        self.assertIn('test "$version" = "$VELA_BETA_VERSION"', build)
        self.assertIn("fedora44_packages=(vela-[0-9]*.fc44.x86_64.rpm)", build)
        self.assertIn("fedora45_packages=(vela-[0-9]*.fc45.x86_64.rpm)", build)
        self.assertIn("Version:        %{vela_version}", spec)
        self.assertIn("Release:        1%{?dist}", spec)
        self.assertIn("SHA256SUMS-fedora-45", build)

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



class BetaVersionDerivation(unittest.TestCase):
    """A release tag and its rehearsal take the same path through every job."""

    WORKFLOWS = ("build.yml", "ubuntu.yml", "fedora.yml")

    def derive(self, ref):
        build = (ROOT / ".github/workflows/build.yml").read_text()
        step = build.split("- name: Derive the Beta version from a release or rehearsal tag\n", 1)[1]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1].split("\n      - ", 1)[0])
        with tempfile.NamedTemporaryFile("r", prefix="vela-output-") as output:
            result = subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                                    env={**os.environ, "REF": ref, "GITHUB_OUTPUT": output.name},
                                    timeout=10)
            return result.returncode, dict(line.split("=", 1) for line in output.read().split())

    def test_release_and_rehearsal_tags_build_the_same_beta_version(self):
        self.assertEqual(self.derive("refs/tags/v0.2.1"), (0, {"beta_version": "0.2.1", "rehearsal": "false"}))
        self.assertEqual(self.derive("refs/tags/rehearsal/v0.2.1"), (0, {"beta_version": "0.2.1", "rehearsal": "true"}))

    def test_branches_and_pull_requests_build_experimental(self):
        for ref in ("refs/heads/main", "refs/heads/release/0.2", "refs/pull/305/merge"):
            with self.subTest(ref=ref):
                self.assertEqual(self.derive(ref), (0, {"beta_version": "", "rehearsal": "false"}))

    def test_malformed_tags_stop_the_pipeline(self):
        for ref in ("refs/tags/v0.2", "refs/tags/v0.2.1-rc1", "refs/tags/rehearsal/v0.2.1a",
                    "refs/tags/rehearsal/0.2.1", "refs/tags/v1.0.0"):
            with self.subTest(ref=ref):
                self.assertNotEqual(self.derive(ref)[0], 0)

    def test_no_job_matches_the_tag_ref_on_its_own(self):
        for name in self.WORKFLOWS:
            text = (ROOT / ".github/workflows" / name).read_text()
            with self.subTest(workflow=name):
                self.assertNotIn("refs/tags/v0.*", text)
                self.assertNotIn("startsWith(github.ref, 'refs/tags/v0.')", text)
                self.assertNotIn("${GITHUB_REF_NAME#v}", text.split("- name: Publish signed Beta release")[0])

    def test_tag_only_jobs_read_the_derived_version(self):
        build = (ROOT / ".github/workflows/build.yml").read_text()
        for job in ("ubuntu-beta", "publish-beta"):
            body = build.split(f"\n  {job}:\n", 1)[1].split("\n  # ", 1)[0]
            with self.subTest(job=job):
                self.assertIn("needs.changes.outputs.beta_version != ''", body)
        for job in ("package-smoke", "fedora-package-smoke"):
            with self.subTest(job=job):
                self.assertRegex(build, rf"(?m)^  {job}:\n    needs: \[changes, ")
        self.assertIn("beta_version: ${{ needs.changes.outputs.beta_version }}", build)


class BetaReleaseNotes(unittest.TestCase):
    def test_notes_are_checked_first_and_published_from_the_changelog(self):
        build = (ROOT / ".github/workflows/build.yml").read_text()
        changes = build.split("\n  changes:\n", 1)[1].split("\n\n  ", 1)[0]
        self.assertIn("if: steps.release.outputs.beta_version != ''", changes)
        notes_command = 'scripts/release-notes.py "$VERSION" --source-ref "$GITHUB_REF_NAME"'
        self.assertIn(notes_command + '\n', changes)
        publish = build.split("\n  publish-beta:\n", 1)[1].split("\n\n  ", 1)[0]
        self.assertIn(notes_command + ' > release-notes.txt', publish)
        self.assertLess(publish.index("release-notes.txt"), publish.index("- name: Publish signed Beta release"))
        self.assertNotIn("--generate-notes", build)


class BetaUpgradeJobs(unittest.TestCase):
    """Upgrades from the previous Beta gate publication; the updater check follows it."""

    DISTROS = ("arch", "fedora-44", "fedora-45", "ubuntu")

    def job(self, workflow, name):
        text = (ROOT / ".github/workflows" / workflow).read_text()
        return text.split(f"\n  {name}:\n", 1)[1].split("\n\n  ", 1)[0]

    def test_package_upgrade_gates_publication_on_every_distro(self):
        upgrade = self.job("build.yml", "beta-upgrade")
        self.assertIn("needs.changes.outputs.beta_version != ''", upgrade)
        self.assertIn("ci-beta-upgrade.py packages", upgrade)
        for distro in self.DISTROS:
            with self.subTest(distro=distro):
                self.assertIn(f"- distro: {distro}\n", upgrade)
        for artifact in ("vela-git", "vela-fedora-rpm", "vela-fedora-45-rpm", "vela-ubuntu-26.10-deb"):
            with self.subTest(artifact=artifact):
                self.assertIn(f"artifact: {artifact}\n", upgrade)
        publish = self.job("build.yml", "publish-beta")
        needs = re.search(r"(?m)^    needs: \[([^\]]+)\]$", publish)[1].split(", ")
        self.assertIn("beta-upgrade", needs)

    def test_updater_check_runs_after_real_releases_only(self):
        check = self.job("build.yml", "beta-update-check")
        self.assertIn("needs: [changes, publish-beta]", check)
        self.assertIn("needs.changes.outputs.rehearsal == 'false'", check)
        self.assertIn("needs.changes.outputs.beta_version != ''", check)
        self.assertIn("uses: ./.github/workflows/beta-update-check.yml", check)

    def test_updater_check_covers_every_distro_and_the_release_tree(self):
        workflow = (ROOT / ".github/workflows/beta-update-check.yml").read_text()
        for distro in self.DISTROS:
            with self.subTest(distro=distro):
                self.assertIn(f"- distro: {distro}\n", workflow)
        self.assertIn("ref: refs/tags/v${{ inputs.version }}", workflow)
        self.assertIn('ci-beta-upgrade.py updater --version "$VERSION" --checkout "$GITHUB_WORKSPACE/release-tree"',
                      workflow)
        self.assertIn("workflow_dispatch:", workflow)


if __name__ == "__main__":
    unittest.main()
