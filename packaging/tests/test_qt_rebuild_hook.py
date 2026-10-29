#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline contracts for Arch's Qt private ABI rebuild notification."""

import configparser
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "packaging/arch/vela-qt-private-rebuild.hook"
NOTICE = ROOT / "packaging/arch/vela-qt-rebuild-notice"
PKGBUILD = ROOT / "packaging/arch/PKGBUILD"


class QtPrivateRebuildHook(unittest.TestCase):
    def test_hook_only_runs_after_upgrading_qt_base(self):
        """Only upgrading the Qt library can trigger the advisory hook."""
        hook = configparser.ConfigParser(interpolation=None)
        self.assertEqual(hook.read(HOOK), [str(HOOK)])
        self.assertEqual(hook["Trigger"]["Operation"], "Upgrade")
        self.assertEqual(hook["Trigger"]["Type"], "Package")
        self.assertEqual(hook["Trigger"]["Target"], "qt6-base")
        self.assertEqual(hook["Action"]["When"], "PostTransaction")
        self.assertEqual(hook["Action"]["Exec"], "/usr/lib/vela/vela-qt-rebuild-notice")

    def test_notice_is_passive_and_documents_forced_rebuild(self):
        """The hook warns but never starts pacman, makepkg or vela-update."""
        result = subprocess.run(["sh", str(NOTICE)], capture_output=True, text=True, check=True)
        self.assertIn("Qt private APIs", result.stderr)
        self.assertIn("vela-update --force --build", result.stderr)
        self.assertFalse(result.stdout)

    def test_package_installs_matching_hook_and_helper(self):
        """The hook and its executable must both be shipped by vela-git."""
        spec = PKGBUILD.read_text()
        self.assertIn('install -Dm644 "$_name/packaging/arch/vela-qt-private-rebuild.hook"', spec)
        self.assertIn('install -Dm755 "$_name/packaging/arch/vela-qt-rebuild-notice"', spec)


if __name__ == "__main__":
    unittest.main()
