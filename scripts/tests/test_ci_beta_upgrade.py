#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline checks for ci-beta-upgrade.py: release choice, distros and its verdicts."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ci_beta_upgrade", ROOT / "scripts/ci-beta-upgrade.py")
upgrade = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upgrade)

ALL = ["SHA256SUMS", "SHA256SUMS-fedora", "SHA256SUMS-fedora-45", "SHA256SUMS-ubuntu"]


def release(tag, assets=ALL, draft=False):
    return {"tag_name": tag, "draft": draft, "assets": [{"name": name} for name in assets]}


class PreviousBeta(unittest.TestCase):
    RELEASES = [release("v0.2.1"), release("v0.2.0"), release("v0.1.10"), release("v0.1.9"),
                release("v0.1.1", ALL[:2] + ALL[3:])]

    def test_newest_older_release_by_numbers_not_text(self):
        self.assertEqual(upgrade.previous_beta("0.2.0", self.RELEASES, "arch"), "0.1.10")
        self.assertEqual(upgrade.previous_beta("0.2.1", self.RELEASES, "arch"), "0.2.0")

    def test_the_version_itself_and_newer_ones_never_count(self):
        self.assertEqual(upgrade.previous_beta("0.2.1", self.RELEASES + [release("v0.3.0")], "ubuntu"), "0.2.0")

    def test_drafts_rehearsals_and_other_tags_are_skipped(self):
        releases = [release("v0.2.0", draft=True), release("rehearsal/v0.2.0"), release("v0.2.0-rc1"),
                    release("alpha-1234567"), release("v0.1.2")]
        self.assertEqual(upgrade.previous_beta("0.2.1", releases, "arch"), "0.1.2")

    def test_a_release_without_this_distros_manifest_is_skipped(self):
        releases = [release("v0.1.2", ALL[:2] + ALL[3:]), release("v0.1.1")]
        self.assertEqual(upgrade.previous_beta("0.2.0", releases, "fedora-45"), "0.1.1")
        self.assertIsNone(upgrade.previous_beta("0.2.0", releases[:1], "fedora-45"))

    def test_first_release_has_nothing_to_upgrade_from(self):
        self.assertIsNone(upgrade.previous_beta("0.1.0", [release("v0.1.0")], "arch"))


class Distros(unittest.TestCase):
    def test_supported_systems(self):
        cases = {("arch", ""): "arch", ("cachyos", ""): "arch", ("fedora", "44"): "fedora-44",
                 ("fedora", "45"): "fedora-45", ("ubuntu", "26.10"): "ubuntu"}
        for (ident, version), name in cases.items():
            with self.subTest(ident=ident, version=version):
                self.assertEqual(upgrade.distro({"ID": ident, "VERSION_ID": version}), name)

    def test_unsupported_systems_fail(self):
        for ident, version in (("fedora", "43"), ("ubuntu", "26.04"), ("debian", "13"), ("", "")):
            with self.subTest(ident=ident, version=version), self.assertRaises(upgrade.UpgradeError):
                upgrade.distro({"ID": ident, "VERSION_ID": version})

    def test_os_release_quotes(self):
        with tempfile.NamedTemporaryFile("w") as f:
            f.write('NAME="Fedora Linux"\nID=fedora\nVERSION_ID="45"\n')
            f.flush()
            self.assertEqual(upgrade.distro(upgrade.os_release(f.name)), "fedora-45")

    def test_candidate_packages_match_only_their_version_and_distro(self):
        names = {"arch": "vela-0.2.1-1-x86_64.pkg.tar.zst", "fedora-44": "vela-0.2.1-1.fc44.x86_64.rpm",
                 "fedora-45": "vela-0.2.1-1.fc45.x86_64.rpm", "ubuntu": "vela_0.2.1_amd64.deb"}
        for name, package in names.items():
            pattern = upgrade.candidate_pattern(name, "0.2.1")
            with self.subTest(name=name):
                self.assertTrue(pattern.fullmatch(package))
                for other in names.values():
                    if other != package:
                        self.assertFalse(pattern.fullmatch(other))
                self.assertFalse(pattern.fullmatch(package.replace("0.2.1", "0.2.10")))
                self.assertFalse(upgrade.candidate_pattern(name, "0.2.10").fullmatch(package))
        self.assertFalse(upgrade.candidate_pattern("arch", "0.2.1").fullmatch(
            "vela-git-0.2.1.r5.g1234567-1-x86_64.pkg.tar.zst"))


class Updater(unittest.TestCase):
    def test_updaters_before_0_2_take_the_documented_one_time_step(self):
        for previous in ("0.1.0", "0.1.2", "0.1.10"):
            with self.subTest(previous=previous):
                self.assertEqual(upgrade.updater_command(previous, "/tree"),
                                 ["sh", "/tree/packaging/vela-update", "--channel", "beta"])

    def test_later_updaters_run_as_installed(self):
        for previous in ("0.2.0", "0.2.3", "0.10.0"):
            with self.subTest(previous=previous):
                self.assertEqual(upgrade.updater_command(previous, "/tree"), ["vela-update", "--channel", "beta"])

    def check(self, report, exit_code):
        with tempfile.TemporaryDirectory() as bindir:
            stub = Path(bindir) / "vela-update"
            stub.write_text(f"#!/bin/sh\nprintf '%s' '{json.dumps(report)}'\nexit {exit_code}\n"
                            if report is not None else f"#!/bin/sh\necho 'vela-update: broken' >&2\nexit {exit_code}\n")
            stub.chmod(0o755)
            with mock.patch.dict(os.environ, {"PATH": f"{bindir}:{os.environ['PATH']}"}):
                upgrade.check_current("0.2.1")

    def test_up_to_date_on_the_new_release_passes(self):
        self.check({"status": "up-to-date", "version": "0.2.1", "error": None}, 0)

    def test_a_failed_check_is_not_up_to_date(self):
        # Plain --check exits 1 both when current and when it failed: an
        # updater that can't see any release must fail here (0.1.x did).
        error = {"status": "error", "version": None,
                 "error": {"code": "not-found", "message": "no official Beta release found"}}
        with self.assertRaisesRegex(upgrade.UpgradeError, "not-found"):
            self.check(error, 1)

    def test_another_update_or_version_fails(self):
        for report in ({"status": "update-available", "version": "0.2.2"},
                       {"status": "up-to-date", "version": "0.2.0"}):
            with self.subTest(report=report), self.assertRaises(upgrade.UpgradeError):
                self.check(report, 0)

    def test_no_json_fails(self):
        with self.assertRaisesRegex(upgrade.UpgradeError, "no JSON"):
            self.check(None, 1)


class InstalledPackage(unittest.TestCase):
    def test_ubuntu_package_verification_requires_no_output_and_zero_exit(self):
        command = ["dpkg", "--verify", "vela"]
        for status, output, error in (
            (0, "??5??????   /usr/bin/vela\n", "reported verification differences"),
            (2, "", "dpkg failed with 2"),
            (0, "", None),
        ):
            with (
                self.subTest(status=status, output=output),
                mock.patch.object(upgrade, "installed_version", return_value="0.2.1"),
                mock.patch.object(upgrade.subprocess, "run") as run,
            ):
                run.side_effect = [subprocess.CompletedProcess(command, status, stdout=output),
                                   subprocess.CompletedProcess([], 0)]
                if error:
                    with self.assertRaisesRegex(upgrade.UpgradeError, error):
                        upgrade.check_installed("ubuntu", "0.2.1")
                    self.assertEqual(run.call_count, 1)
                else:
                    upgrade.check_installed("ubuntu", "0.2.1")
                    self.assertEqual(run.call_args.args[0], ["vela-update", "--channel", "beta", "--help"])
                self.assertEqual(run.call_args_list[0].args[0], command)
                self.assertEqual(run.call_args_list[0].kwargs["stdout"], subprocess.PIPE)


class Downloads(unittest.TestCase):
    def serve(self, files):
        def fetch(url, token=None):
            return files[url.rsplit("/", 1)[1]]
        return mock.patch.object(upgrade, "fetch", fetch)

    def test_package_matching_its_manifest_is_saved(self):
        data = b"package"
        manifest = f"{hashlib.sha256(data).hexdigest()}  vela_0.2.0_amd64.deb\n".encode()
        with tempfile.TemporaryDirectory() as d, self.serve({"SHA256SUMS-ubuntu": manifest,
                                                             "vela_0.2.0_amd64.deb": data}):
            path = upgrade.download_previous("0.2.0", "ubuntu", d)
            self.assertEqual(path.read_bytes(), data)

    def test_tampered_package_is_refused(self):
        manifest = f"{hashlib.sha256(b'package').hexdigest()}  vela_0.2.0_amd64.deb\n".encode()
        with tempfile.TemporaryDirectory() as d, self.serve({"SHA256SUMS-ubuntu": manifest,
                                                             "vela_0.2.0_amd64.deb": b"other"}):
            with self.assertRaisesRegex(upgrade.UpgradeError, "doesn't match"):
                upgrade.download_previous("0.2.0", "ubuntu", d)
            self.assertEqual(list(Path(d).iterdir()), [])

    def test_manifest_must_list_one_package(self):
        manifest = b"aa  one.deb\nbb  two.deb\n"
        with tempfile.TemporaryDirectory() as d, self.serve({"SHA256SUMS-ubuntu": manifest}):
            with self.assertRaisesRegex(upgrade.UpgradeError, "exactly one"):
                upgrade.download_previous("0.2.0", "ubuntu", d)

    def test_token_is_sent_only_to_the_github_api(self):
        seen = []

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self):
                return b"[]"

        def urlopen(request, timeout):
            seen.append((request.full_url, request.get_header("Authorization")))
            return Response()

        with mock.patch.object(upgrade.urllib.request, "urlopen", urlopen):
            upgrade.fetch("https://api.github.com/repos/vela-desktop/vela/releases", "secret")
            upgrade.fetch("https://github.com/vela-desktop/vela/releases/download/v0.2.0/SHA256SUMS", "secret")
        self.assertEqual(seen[0][1], "Bearer secret")
        self.assertIsNone(seen[1][1])


if __name__ == "__main__":
    unittest.main()
