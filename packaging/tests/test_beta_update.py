#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Vela contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline contracts for the numbered Beta release channel."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
UPDATER = ROOT / "packaging/vela-update-beta"
COMMIT = "b" * 40
MOCK = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
if name == "curl":
    if args[-1].endswith("/releases?per_page=100"):
        print(os.environ["TEST_RELEASE_LIST"])
    else:
        sys.exit(22)  # No manifest may be installed without a verified release.
elif name == "git":
    if args[0] == "ls-remote":
        print(os.environ["TEST_COMMIT"] + "\trefs/tags/" + args[-2].split("/")[-1])
    else:
        sys.exit(2)
elif name == "pacman":
    if args[:2] != ["-Q", "vela"] or not os.environ.get("TEST_INSTALLED"):
        sys.exit(1)
    print("vela " + os.environ["TEST_INSTALLED"])
elif name == "flock":
    sys.exit(0)
elif name == "sudo":
    with open(os.environ["TEST_UNEXPECTED_SUDO"], "w") as f:
        f.write("called")
    sys.exit(99)
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
        for name in ("curl", "git", "pacman", "flock", "sudo"):
            path = bindir / name
            path.write_text(MOCK)
            path.chmod(0o755)
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
                    "TEST_INSTALLED": ""}

    def run_update(self, *args):
        return subprocess.run(["sh", str(UPDATER), *args], env=self.env,
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


if __name__ == "__main__":
    unittest.main()
