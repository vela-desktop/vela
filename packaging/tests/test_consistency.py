#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Facts the updater states in more than one place must agree."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text()


class RepositoryIdentity(unittest.TestCase):
    """The official repository is named by both channels' scripts, the daemon
    and the packages; moving it must not leave one of them behind."""

    def setUp(self):
        match = re.search(r"^REPO=([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)$",
                          read("packaging/vela-update-beta"), re.M)
        self.assertIsNotNone(match, "vela-update-beta must set REPO=owner/name")
        self.repo = match.group(1)
        self.https = f"https://github.com/{self.repo}"

    def test_experimental_default_source(self):
        update = read("packaging/vela-update")
        self.assertIn(f'DEFAULT_SOURCE="{self.https}.git"', update)
        # githubHTTPS canonicalizes the official URL's aliases, lowercase.
        aliases = re.search(r"^\s*(https://github\.com/[^)\n]*)\)$", update, re.M)
        self.assertIsNotNone(aliases)
        lower = self.repo.lower()
        for alias in aliases.group(1).split("|"):
            self.assertTrue(alias.endswith((lower, lower + ".git")), alias)
        # The bootstrap accepts the official origin with or without .git.
        self.assertIn(f'"$DEFAULT_SOURCE"|{self.https})', update)

    def test_daemon_default_source(self):
        self.assertIn(f'"{self.https}.git"', read("updates/src/state.c"))

    def test_package_metadata(self):
        for path in ("packaging/arch/PKGBUILD", "packaging/arch/PKGBUILD.beta"):
            self.assertIn(f'url="{self.https}"', read(path), path)
        self.assertRegex(read("packaging/fedora/vela.spec"),
                         r"(?m)^URL:\s+" + re.escape(self.https) + "$")


class UbuntuBuildDepends(unittest.TestCase):
    def test_workflow_installs_the_listed_packages(self):
        listed = [line.split("#", 1)[0].strip()
                  for line in read("packaging/ubuntu/build-depends").splitlines()]
        listed = [name for name in listed if name]
        self.assertEqual(len(listed), len(set(listed)), "duplicate package")
        for name in listed:
            self.assertRegex(name, r"^[a-z0-9][a-z0-9.+-]+$")

        workflow = read(".github/workflows/ubuntu.yml")
        command = re.search(r"apt-get install -y --no-install-recommends \\\n(.*?)\n\s*#",
                            workflow, re.S)
        self.assertIsNotNone(command, "ubuntu.yml's dependency install step")
        installed = command.group(1).replace("\\", " ").split()
        self.assertEqual(sorted(installed), sorted(listed))


if __name__ == "__main__":
    unittest.main()
