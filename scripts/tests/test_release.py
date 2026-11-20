#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Run release.py against a temporary repository, a bare remote and a fake gh."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/release.py"
# Releases live in a JSON file: {tag: {"isDraft": bool}}.
GH_STUB = r'''#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
state = os.environ["TEST_GH_STATE"]
releases = json.load(open(state))
with open(os.environ["TEST_GH_LOG"], "a") as log:
    log.write(json.dumps(args) + "\n")
if args[:2] == ["release", "view"]:
    if args[2] not in releases:
        sys.exit("release not found")
    print(json.dumps(releases[args[2]]))
elif args[:2] == ["release", "delete"]:
    del releases[args[2]]
    json.dump(releases, open(state, "w"))
else:
    sys.exit("Unexpected gh call: " + repr(args))
'''


class Release(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="vela-release-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.remote = self.root / "remote.git"
        self.work = self.root / "work"
        bindir = self.root / "bin"
        bindir.mkdir()
        (bindir / "gh").write_text(GH_STUB)
        (bindir / "gh").chmod(0o755)
        self.state = self.root / "releases.json"
        self.state.write_text("{}")
        self.log = self.root / "gh.jsonl"
        self.log.touch()
        self.env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}",
                    "TEST_GH_STATE": str(self.state), "TEST_GH_LOG": str(self.log),
                    "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
                    "GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "test@example.invalid",
                    "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "test@example.invalid"}
        self.git("init", "--quiet", "--bare", "--initial-branch=main", str(self.remote), cwd=self.root)
        self.git("init", "--quiet", "--initial-branch=main", str(self.work), cwd=self.root)
        self.git("remote", "add", "origin", str(self.remote))
        self.main_commit = self.commit("0.2.0")
        self.git("push", "--quiet", "origin", "main")
        self.git("switch", "--quiet", "-c", "release/0.2")
        self.old_patch = self.commit("0.2.0", "a fix")
        self.patch = self.commit("0.2.1", "bump")
        self.git("push", "--quiet", "origin", "release/0.2")

    def git(self, *args, cwd=None):
        return subprocess.run(["git", *args], cwd=cwd or self.work, env=self.env, check=True,
                              capture_output=True, text=True).stdout.strip()

    def commit(self, version, message=None):
        (self.work / "CMakeLists.txt").write_text(
            f"cmake_minimum_required(VERSION 3.22)\nproject(vela\n    VERSION {version}\n    LANGUAGES C CXX)\n")
        (self.work / "change").write_text(message or version)
        self.git("add", ".")
        self.git("commit", "--quiet", "-m", message or version)
        return self.git("rev-parse", "HEAD")

    def release(self, *args):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), *args], cwd=self.work, env=self.env,
                              capture_output=True, text=True, timeout=30)

    def remote_tags(self):
        out = self.git("ls-remote", "--tags", "origin")
        return {ref.removeprefix("refs/tags/"): sha for sha, ref in
                (line.split("\t") for line in out.splitlines())}

    def releases(self):
        return json.loads(self.state.read_text())

    def test_patch_rehearsal_tags_the_release_branch_tip(self):
        result = self.release("rehearse", "0.2.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.remote_tags(), {"rehearsal/v0.2.1": self.patch})
        self.assertIn("scripts/release.py clean 0.2.1", result.stdout)

    def test_minor_rehearsal_tags_main(self):
        result = self.release("rehearse", "0.2.0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.remote_tags(), {"rehearsal/v0.2.0": self.main_commit})

    def test_project_version_must_match(self):
        result = self.release("rehearse", "0.2.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("project(VERSION 0.2.1)", result.stderr)
        self.assertEqual(self.remote_tags(), {})

    def test_named_commit_must_be_on_the_release_branch(self):
        self.git("switch", "--quiet", "-c", "elsewhere")
        stray = self.commit("0.2.1", "not merged")
        result = self.release("rehearse", "0.2.1", "--commit", stray)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is not on origin/release/0.2", result.stderr)
        self.assertEqual(self.remote_tags(), {})

    def test_named_commit_needs_the_version_bump(self):
        result = self.release("rehearse", "0.2.1", "--commit", self.old_patch)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("merge the version bump", result.stderr)

    def test_a_published_version_is_not_rehearsed(self):
        self.git("push", "--quiet", "origin", f"{self.patch}:refs/tags/v0.2.1")
        result = self.release("rehearse", "0.2.1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("already tagged", result.stderr)
        self.assertNotIn("rehearsal/v0.2.1", self.remote_tags())

    def test_a_new_rehearsal_replaces_the_previous_one(self):
        self.git("push", "--quiet", "origin", f"{self.old_patch}:refs/tags/rehearsal/v0.2.1")
        self.state.write_text(json.dumps({"rehearsal/v0.2.1": {"isDraft": True}}))
        result = self.release("rehearse", "0.2.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.releases(), {})
        self.assertEqual(self.remote_tags(), {"rehearsal/v0.2.1": self.patch})

    def test_a_published_rehearsal_release_is_never_deleted(self):
        self.git("push", "--quiet", "origin", f"{self.patch}:refs/tags/rehearsal/v0.2.1")
        self.state.write_text(json.dumps({"rehearsal/v0.2.1": {"isDraft": False}}))
        for command in ("rehearse", "clean"):
            with self.subTest(command=command):
                result = self.release(command, "0.2.1")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("not a draft", result.stderr)
                self.assertIn("rehearsal/v0.2.1", self.releases())
                self.assertIn("rehearsal/v0.2.1", self.remote_tags())

    def test_clean_deletes_the_draft_and_the_tag(self):
        self.git("push", "--quiet", "origin", f"{self.patch}:refs/tags/rehearsal/v0.2.1")
        self.state.write_text(json.dumps({"rehearsal/v0.2.1": {"isDraft": True}}))
        result = self.release("clean", "0.2.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.releases(), {})
        self.assertEqual(self.remote_tags(), {})

    def test_clean_without_a_rehearsal_does_nothing(self):
        result = self.release("clean", "0.2.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        deletes = [line for line in self.log.read_text().splitlines() if '"delete"' in line]
        self.assertEqual(deletes, [])

    def test_clean_leaves_the_real_tag_alone(self):
        self.git("push", "--quiet", "origin", f"{self.patch}:refs/tags/v0.2.1")
        self.state.write_text(json.dumps({"v0.2.1": {"isDraft": False}}))
        self.assertEqual(self.release("clean", "0.2.1").returncode, 0)
        self.assertEqual(self.remote_tags(), {"v0.2.1": self.patch})
        self.assertIn("v0.2.1", self.releases())

    def test_malformed_versions_are_refused(self):
        for version in ("0.2", "v0.2.1", "0.2.1a", "1.0.0", "0.2.1-rc1"):
            for command in ("rehearse", "clean"):
                with self.subTest(version=version, command=command):
                    result = self.release(command, version)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("not a Beta version", result.stderr)
        self.assertEqual(self.remote_tags(), {})


if __name__ == "__main__":
    unittest.main()
