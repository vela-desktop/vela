#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Vela contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline contracts for the numbered Beta release channel."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
UPDATER = ROOT / "packaging/vela-update-beta"
COMMIT = "b" * 40
MOCK = r'''#!/usr/bin/env python3
import hashlib, json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
if name == "curl":
    url = args[-1]
    if "/releases?per_page=100&page=" in url:
        releases = json.loads(os.environ["TEST_RELEASE_LIST"])
        page = int(url.rsplit("page=", 1)[1])
        print(json.dumps(releases[(page - 1) * 100:page * 100]))
    elif os.environ.get("TEST_SIGNED_MANIFEST") != "1":
        sys.exit(22)
    else:
        payload = b"verified-beta-test-payload"
        pkg = os.environ["TEST_PACKAGE"]
        digest = hashlib.sha256(payload).hexdigest()
        if os.environ.get("TEST_BAD_DIGEST"):
            digest = "0" * 64
        output = pathlib.Path(args[args.index("-o") + 1])
        if url.endswith("/" + os.environ["TEST_MANIFEST"]):
            output.write_text(digest + "  " + pkg + "\n")
        elif url.endswith("/" + pkg):
            output.write_bytes(payload + (b"bad" if os.environ.get("TEST_CORRUPT") else b""))
        else:
            sys.exit(22)
elif name == "git":
    if args[0] == "ls-remote":
        print(os.environ["TEST_COMMIT"] + "\trefs/tags/" + args[-2].split("/")[-1])
    else:
        sys.exit(2)
elif name == "pacman":
    if args[:2] != ["-Q", "vela"] or not os.environ.get("TEST_INSTALLED"):
        sys.exit(1)
    print("vela " + os.environ["TEST_INSTALLED"])
elif name == "rpm":
    if args[-1] != "vela" or not os.environ.get("TEST_INSTALLED"):
        sys.exit(1)
    print(os.environ["TEST_INSTALLED"])
elif name == "dpkg-query":
    if args[-1] != "vela" or not os.environ.get("TEST_INSTALLED"):
        sys.exit(1)
    print("install ok installed\t" + os.environ["TEST_INSTALLED"])
elif name in ("flock", "dnf", "apt-get", "cosign"):
    sys.exit(0)
elif name == "sudo":
    with open(os.environ["TEST_UNEXPECTED_SUDO"], "w") as f:
        json.dump(args, f)
    sys.exit(1 if os.environ.get("TEST_FAIL_INSTALL") else 0)
else:
    sys.exit("Unexpected executable: " + name)
'''


class BetaUpdateTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="vela-beta-updater-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        bindir = self.root / "bin"
        bindir.mkdir()
        for name in ("curl", "git", "pacman", "rpm", "dpkg-query",
                     "dnf", "apt-get", "cosign", "flock", "sudo"):
            path = bindir / name
            path.write_text(MOCK)
            path.chmod(0o755)
        # Exercise the release updater on a synthetic OS, not the CI host.
        scripts = self.root / "scripts"
        scripts.mkdir()
        self.script = scripts / "vela-update-beta"
        self.os_release = self.root / "os-release"
        self.os_release.write_text("ID=arch\n")
        original = UPDATER.read_text()
        self.assertEqual(original.count("/etc/os-release"), 2)
        self.script.write_text(original.replace("/etc/os-release", str(self.os_release)))
        self.verified = self.root / "verified.json"
        verifier = scripts / "verify-attestation.py"
        verifier.write_text('#!/usr/bin/env python3\n'
                            'import json,os,sys,pathlib\n'
                            'pathlib.Path(os.environ["TEST_VERIFIED"]).write_text(json.dumps(sys.argv[1:]))\n'
                            'sys.exit(1 if os.environ.get("TEST_REJECT_ATTEST") else 0)\n')
        verifier.chmod(0o755)
        self.calls = self.root / "sudo-called"
        releases = [
            {"tag_name": "v0.1.9", "prerelease": True, "draft": False},
            {"tag_name": "v0.1.11", "prerelease": True, "draft": False},
            {"tag_name": "v0.2.0", "prerelease": True, "draft": True},
            {"tag_name": "v1.0.0", "prerelease": False, "draft": False},
            {"tag_name": "v0.9.0-rc.1", "prerelease": True, "draft": False},
        ]
        self.env = {**os.environ,
                    "PATH": f"{bindir}:{os.environ['PATH']}",
                    "HOME": str(self.root),
                    "XDG_CACHE_HOME": str(self.root / "cache"),
                    "TEST_RELEASE_LIST": json.dumps(releases),
                    "TEST_COMMIT": COMMIT,
                    "TEST_UNEXPECTED_SUDO": str(self.calls),
                    "TEST_VERIFIED": str(self.verified),
                    "TEST_SIGNED_MANIFEST": "0",
                    "TEST_PACKAGE": "vela-0.1.11-1-x86_64.pkg.tar.zst",
                    "TEST_MANIFEST": "SHA256SUMS",
                    "TEST_INSTALLED": ""}

    def run_update(self, *args):
        return subprocess.run(["sh", str(self.script), *args], env=self.env,
                              capture_output=True, text=True, timeout=20)

    def test_latest_beta_selected_by_numeric_semver_and_drafts_ignored(self):
        result = self.run_update("--channel", "beta", "--check", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["status"], "update-available")
        self.assertEqual(payload["channel"], "beta")
        self.assertEqual(payload["version"], "0.1.11")
        self.assertEqual(payload["tag"], "v0.1.11")
        self.assertEqual(payload["commit"], COMMIT)
        self.assertEqual(payload["schema_version"], 1)
        self.assertFalse(self.calls.exists())

    def test_same_installed_release_is_up_to_date(self):
        self.env["TEST_INSTALLED"] = "0.1.11-1"
        result = self.run_update("--check", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "up-to-date")
        self.assertFalse(self.calls.exists())

    def test_human_check_exit_codes(self):
        self.assertEqual(self.run_update("--check").returncode, 0)
        self.env["TEST_INSTALLED"] = "0.1.11-1"
        self.assertEqual(self.run_update("--check").returncode, 1)

    def test_pinned_release_does_not_advance(self):
        result = self.run_update("--check", "--json", "--tag=v0.1.9")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["tag"], "v0.1.9")

    def test_no_beta_release_does_not_install_anything(self):
        self.env["TEST_RELEASE_LIST"] = "[]"
        result = self.run_update("--check", "--json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no official Beta", result.stderr)
        self.assertFalse(self.calls.exists())

    def test_rejects_unsupported_option(self):
        result = self.run_update("--channel", "experimental", "--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.calls.exists())

    def test_missing_signed_manifest_blocks_install(self):
        result = self.run_update()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.calls.exists())


    def configure_os(self, distro, version=""):
        self.os_release.write_text("ID=" + distro + "\n" +
                                   ('VERSION_ID="' + version + '"\n' if version else ""))
        if distro == "fedora":
            self.env["TEST_PACKAGE"] = "vela-0.1.11-1.fc44.x86_64.rpm"
            self.env["TEST_MANIFEST"] = "SHA256SUMS-fedora"
        elif distro == "ubuntu":
            self.env["TEST_PACKAGE"] = "vela_0.1.11_amd64.deb"
            self.env["TEST_MANIFEST"] = "SHA256SUMS-ubuntu"

    def test_beta_release_after_first_page_is_discovered(self):
        releases = [
            {"tag_name": f"v0.1.{n}", "prerelease": True, "draft": False}
            for n in range(100)
        ]
        releases.append({"tag_name": "v0.99.0", "prerelease": True, "draft": False})
        self.env["TEST_RELEASE_LIST"] = json.dumps(releases)
        result = self.run_update("--check", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["tag"], "v0.99.0")

    def test_signed_arch_beta_installs_successfully(self):
        self.env["TEST_SIGNED_MANIFEST"] = "1"
        result = self.run_update()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.calls.read_text())[:2], ["pacman", "-U"])
        verified = json.loads(self.verified.read_text())
        self.assertEqual(verified[verified.index("--release-tag") + 1], "v0.1.11")
        self.assertEqual(verified[verified.index("--commit") + 1], COMMIT)

    def test_fedora_uses_dnf_even_if_pacman_exists(self):
        self.configure_os("fedora", "44")
        self.env["TEST_SIGNED_MANIFEST"] = "1"
        result = self.run_update()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.calls.read_text())[:4],
                         ["dnf", "install", "-y", "--allowerasing"])

    def test_ubuntu_uses_apt_even_if_pacman_exists(self):
        self.configure_os("ubuntu", "26.10")
        self.env["TEST_SIGNED_MANIFEST"] = "1"
        result = self.run_update()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.calls.read_text())[:4],
                         ["apt-get", "install", "-y", "--allow-downgrades"])

    def test_unsupported_distributions_and_versions_are_rejected(self):
        for distro, version in (("fedora", "43"), ("ubuntu", "24.04"),
                                ("debian", "13")):
            with self.subTest(distro=distro):
                self.configure_os(distro, version)
                result = self.run_update("--check")
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.calls.exists())

    def test_newer_installed_beta_is_not_silently_downgraded(self):
        self.env["TEST_INSTALLED"] = "0.2.0-1"
        result = self.run_update("--check", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "downgrade-available")
        result = self.run_update()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refusing implicit downgrade", result.stderr)
        self.assertFalse(self.calls.exists())

    def test_explicit_pinned_downgrade_is_allowed(self):
        self.env["TEST_INSTALLED"] = "0.2.0-1"
        self.env["TEST_SIGNED_MANIFEST"] = "1"
        result = self.run_update("--tag=v0.1.11")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.calls.exists())

    def test_corrupted_download_fails_before_attestation(self):
        self.env["TEST_SIGNED_MANIFEST"] = "1"
        self.env["TEST_CORRUPT"] = "1"
        self.assertNotEqual(self.run_update().returncode, 0)
        self.assertFalse(self.calls.exists())
        self.assertFalse(self.verified.exists())

    def test_invalid_checksum_fails_before_install(self):
        self.env["TEST_SIGNED_MANIFEST"] = "1"
        self.env["TEST_BAD_DIGEST"] = "1"
        self.assertNotEqual(self.run_update().returncode, 0)
        self.assertFalse(self.calls.exists())

    def test_rejected_signature_fails_before_install(self):
        self.env["TEST_SIGNED_MANIFEST"] = "1"
        self.env["TEST_REJECT_ATTEST"] = "1"
        self.assertNotEqual(self.run_update().returncode, 0)
        self.assertTrue(self.verified.exists())
        self.assertFalse(self.calls.exists())

    def test_package_manager_failure_is_not_reported_as_success(self):
        self.env["TEST_SIGNED_MANIFEST"] = "1"
        self.env["TEST_FAIL_INSTALL"] = "1"
        self.assertNotEqual(self.run_update().returncode, 0)
        self.assertTrue(self.calls.exists())


if __name__ == "__main__":
    unittest.main()
