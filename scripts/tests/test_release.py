#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Run release.py against a temporary repository, a bare remote and a fake gh."""

import datetime
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/release.py"
spec = importlib.util.spec_from_file_location("release", SCRIPT)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
# Releases live in a JSON file: {tag: {"isDraft": bool}}.
GH_STUB = r'''#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
state = os.environ["TEST_GH_STATE"]
releases = json.load(open(state))
with open(os.environ["TEST_GH_LOG"], "a") as log:
    log.write(json.dumps(args) + "\n")
if args[:2] == ["api", "graphql"]:
    fields = dict(arg.split("=", 1) for arg in args[2:] if "=" in arg)
    if os.environ.get("TEST_GH_LOOKUP_FAIL"):
        # Even a partial response with a null release is inconclusive.
        print(json.dumps({"data": {"repository": {"release": None}}}))
        sys.exit("API rate limit exceeded")
    print(json.dumps({"data": {"repository": {"release": releases.get(fields["tag"])}}}))
elif args[:2] == ["release", "view"]:
    if args[2] not in releases:
        sys.exit("release not found")
    print(json.dumps(releases[args[2]]))
elif args[:2] == ["release", "delete"]:
    del releases[args[2]]
    json.dump(releases, open(state, "w"))
elif args[:2] == ["run", "list"]:
    runs = json.load(open(os.environ["TEST_GH_RUNS"])).get(args[args.index("--workflow") + 1], [])
    if "--commit" in args:
        runs = [r for r in runs if r["headSha"] == args[args.index("--commit") + 1]]
    if "--branch" in args:
        runs = [r for r in runs if r.get("headBranch") == args[args.index("--branch") + 1]]
    print(json.dumps(runs[:int(args[args.index("--limit") + 1])]))
elif args[:2] == ["workflow", "run"]:
    pass
elif args[:2] == ["pr", "list"]:
    head = args[args.index("--head") + 1]
    base = args[args.index("--base") + 1]
    prs = json.load(open(os.environ["TEST_GH_PRS"]))
    print(json.dumps(prs.get(f"{base}:{head}", [])))
elif args[:2] == ["pr", "create"]:
    head = args[args.index("--head") + 1]
    base = args[args.index("--base") + 1]
    if os.environ.get("TEST_GH_FAIL_CREATE") == head:
        sys.exit("could not create pull request")
    prs_path = os.environ["TEST_GH_PRS"]
    prs = json.load(open(prs_path))
    key = f"{base}:{head}"
    if key in prs:
        sys.exit("a pull request already exists")
    url = "https://github.com/vela-desktop/vela/pull/999"
    prs[key] = [{"url": url, "isCrossRepository": False}]
    json.dump(prs, open(prs_path, "w"))
    print(url)
else:
    sys.exit("Unexpected gh call: " + repr(args))
'''


CHANGELOG = """# Changelog

Intro.

## Unreleased

- The taskbar can sit on any edge.

- Context menus lost "Show more options".

## 0.1.2 — 16 November 2026

- Fedora 45.
"""


class Fixture(unittest.TestCase):
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
        self.runs = self.root / "runs.json"
        self.runs.write_text("{}")
        prs = self.root / "pulls.json"
        prs.write_text("{}")
        self.env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}",
                    "TEST_GH_STATE": str(self.state), "TEST_GH_LOG": str(self.log),
                    "TEST_GH_RUNS": str(self.runs), "TEST_GH_PRS": str(prs),
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
        (self.work / "CHANGELOG.md").write_text(CHANGELOG)
        (self.work / "packaging/arch").mkdir(parents=True, exist_ok=True)
        (self.work / "packaging/arch/PKGBUILD.beta").write_text(
            "pkgname=vela\npkgver=${VELA_RELEASE_VERSION:-0.1.2}\npkgrel=1\n")
        (self.work / "change").write_text(message or version)
        self.git("add", ".")
        self.git("commit", "--quiet", "-m", message or version)
        return self.git("rev-parse", "HEAD")

    def release(self, *args):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), *args], cwd=self.work, env=self.env,
                              capture_output=True, text=True, timeout=30)

    def remote_tags(self):
        out = self.git("ls-remote", "--tags", "origin")
        tags = {}
        for sha, ref in (line.split("\t") for line in out.splitlines()):
            name = ref.removeprefix("refs/tags/")
            # An annotated tag's commit is the peeled ^{} line.
            if name.endswith("^{}"):
                tags[name[:-3]] = sha
            else:
                tags.setdefault(name, sha)
        return tags

    def releases(self):
        return json.loads(self.state.read_text())

    def gh_calls(self, *prefix):
        calls = [json.loads(line) for line in self.log.read_text().splitlines()]
        return [c for c in calls if c[:len(prefix)] == list(prefix)]


class Release(Fixture):
    def test_rehearsal_starts_both_gpu_suites_on_the_tag(self):
        self.assertEqual(self.release("rehearse", "0.2.1").returncode, 0)
        started = self.gh_calls("workflow", "run")
        self.assertEqual(sorted(c[2] for c in started), ["gpu-amd.yml", "gpu-nvidia.yml"])
        for call in started:
            self.assertEqual(call[call.index("--ref") + 1], "rehearsal/v0.2.1")

    def test_rehearsal_without_gpu(self):
        self.assertEqual(self.release("rehearse", "0.2.1", "--no-gpu").returncode, 0)
        self.assertEqual(self.gh_calls("workflow", "run"), [])

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

    def test_failed_release_lookup_keeps_the_existing_tag(self):
        tag = "rehearsal/v0.2.1"
        self.git("push", "--quiet", "origin", f"{self.old_patch}:refs/tags/{tag}")
        self.state.write_text(json.dumps({tag: {"isDraft": True}}))
        self.env["TEST_GH_LOOKUP_FAIL"] = "1"
        for command in ("clean", "rehearse"):
            with self.subTest(command=command):
                result = self.release(command, "0.2.1")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("API rate limit exceeded", result.stderr)
                self.assertEqual(self.remote_tags(), {tag: self.old_patch})
                self.assertEqual(self.releases(), {tag: {"isDraft": True}})

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
            for command in ("prepare", "rehearse", "status", "tag", "clean"):
                with self.subTest(version=version, command=command):
                    result = self.release(command, version)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("not a Beta version", result.stderr)
        self.assertEqual(self.remote_tags(), {})



class Changelog(unittest.TestCase):
    DAY = datetime.date(2026, 12, 3)

    def test_unreleased_entries_move_under_the_version(self):
        text = release.release_changelog(CHANGELOG, "0.2.1", self.DAY)
        self.assertEqual(text, CHANGELOG.replace(
            "## Unreleased\n\n", "## Unreleased\n\n## 0.2.1 — 3 December 2026\n\n"))

    def test_unreleased_as_the_last_section(self):
        text = release.release_changelog("# Changelog\n\n## Unreleased\n\n- One.\n", "0.2.0", self.DAY)
        self.assertEqual(text, "# Changelog\n\n## Unreleased\n\n## 0.2.0 — 3 December 2026\n\n- One.\n")

    def test_nothing_unreleased_or_already_released_is_refused(self):
        for text, error in (("# C\n\n## Unreleased\n\n## 0.2.0 — 1 December 2026\n", "nothing under"),
                            ("# C\n\n## 0.2.0 — 1 December 2026\n", "no \"## Unreleased\""),
                            (CHANGELOG, "already has")):
            version = "0.1.2" if error == "already has" else "0.2.1"
            with self.subTest(error=error), self.assertRaisesRegex(release.ReleaseError, error):
                release.release_changelog(text, version, self.DAY)

    def test_project_version_is_the_only_line_changed(self):
        cmake = "cmake_minimum_required(VERSION 3.22)\nproject(vela\n    VERSION 0.2.0\n    LANGUAGES C)\n"
        self.assertEqual(release.bump_project_version(cmake, "0.2.1"), cmake.replace("0.2.0", "0.2.1"))


class GpuSuiteNames(unittest.TestCase):
    def test_full_suites_keep_the_name_status_expects(self):
        for workflow, title in release.GPU_SUITES.items():
            text = (ROOT / ".github/workflows" / workflow).read_text()
            with self.subTest(workflow=workflow):
                self.assertIn(f"\nname: {title}\n", text)
                # With no test filter the run is titled exactly like the
                # workflow; a filter appends " - only <regex>".
                self.assertIn(f"\nrun-name: {title}${{{{ inputs.tests && format(' - only {{0}}', inputs.tests) || '' }}}}\n",
                              text)
                self.assertIn("      tests:\n", text)


class Prepare(Fixture):
    def test_patch_bump_opens_a_pull_request_on_the_release_branch(self):
        self.git("switch", "--quiet", "release/0.2")
        result = self.release("prepare", "0.2.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        branch = self.git("ls-remote", "origin", "refs/heads/prepare/v0.2.2").split()[0]
        self.assertEqual(self.git("rev-parse", f"{branch}^"), self.patch)
        changed = self.git("diff", "--name-only", self.patch, branch).split()
        self.assertEqual(sorted(changed), ["CHANGELOG.md", "CMakeLists.txt"])
        self.assertIn("VERSION 0.2.2", self.git("show", f"{branch}:CMakeLists.txt"))
        self.assertIn("## 0.2.2 — ", self.git("show", f"{branch}:CHANGELOG.md"))
        self.assertIn("Signed-off-by: Test", self.git("log", "-1", "--format=%B", branch))
        created = self.gh_calls("pr", "create")
        self.assertEqual(len(created), 1)
        self.assertEqual(created[0][created[0].index("--base") + 1], "release/0.2")
        self.assertEqual(created[0][created[0].index("--head") + 1], "prepare/v0.2.2")
        # The maintainer's own checkout is left alone.
        self.assertEqual(self.git("status", "--porcelain"), "")
        self.assertEqual(self.git("worktree", "list").count("\n"), 0)

    def test_minor_release_keeps_mains_version(self):
        result = self.release("prepare", "0.2.0")
        self.assertEqual(result.returncode, 0, result.stderr)
        branch = self.git("ls-remote", "origin", "refs/heads/prepare/v0.2.0").split()[0]
        self.assertEqual(self.git("diff", "--name-only", self.main_commit, branch), "CHANGELOG.md")

    def test_no_bump_backwards_or_onto_a_published_version(self):
        self.assertIn("already at 0.2.1", self.release("prepare", "0.2.1").stderr)
        self.assertIn("main is at 0.2.0", self.release("prepare", "0.3.0").stderr)
        self.git("push", "--quiet", "origin", f"{self.patch}:refs/tags/v0.2.2")
        self.assertIn("already tagged", self.release("prepare", "0.2.2").stderr)
        self.assertEqual(self.gh_calls("pr", "create"), [])

    def test_prepare_reuses_the_pushed_branch_and_then_the_open_pr(self):
        head = "prepare/v0.2.2"
        ref = f"refs/heads/{head}"
        self.env["TEST_GH_FAIL_CREATE"] = head
        failed = self.release("prepare", "0.2.2")
        self.assertIn("could not create pull request", failed.stderr)
        del self.env["TEST_GH_FAIL_CREATE"]
        pushed = self.git("ls-remote", "origin", ref)
        self.assertTrue(pushed)
        self.commit("0.2.1", "advance the base")
        self.git("push", "--quiet", "origin", "release/0.2")

        retry = self.release("prepare", "0.2.2")
        self.assertEqual(retry.returncode, 0, retry.stderr)
        self.assertEqual(self.git("ls-remote", "origin", ref), pushed)
        creates = self.gh_calls("pr", "create")
        again = self.release("prepare", "0.2.2")
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertIn("Already open:", again.stdout)
        self.assertEqual(self.gh_calls("pr", "create"), creates)

    def test_prepare_accepts_the_original_changelog_date(self):
        self.git("switch", "--quiet", "-c", "prepare-fixture")
        cmake = self.work / "CMakeLists.txt"
        cmake.write_text(release.bump_project_version(cmake.read_text(), "0.2.2"))
        changelog = self.work / "CHANGELOG.md"
        changelog.write_text(release.release_changelog(changelog.read_text(), "0.2.2", datetime.date(2020, 1, 1)))
        self.git("commit", "--quiet", "-sam", "prepare on an earlier day")
        self.git("push", "--quiet", "origin", "HEAD:refs/heads/prepare/v0.2.2")

        retry = self.release("prepare", "0.2.2")
        self.assertEqual(retry.returncode, 0, retry.stderr)
        self.assertEqual(self.git("ls-remote", "origin", "refs/heads/prepare/v0.2.2").split()[0],
                         self.git("rev-parse", "HEAD"))

    def test_prepare_refuses_to_overwrite_an_unrelated_remote_head(self):
        ref = "refs/heads/prepare/v0.2.2"
        self.git("switch", "--quiet", "-c", "unrelated")
        other = self.commit("0.2.1", "unexpected change")
        self.git("push", "--quiet", "origin", f"{other}:{ref}")
        result = self.release("prepare", "0.2.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("origin/prepare/v0.2.2 does not contain the intended change", result.stderr)
        self.assertEqual(self.git("ls-remote", "origin", ref).split()[0], other)
        self.assertEqual(self.gh_calls("pr", "create"), [])


class StatusAndTag(Fixture):
    def setUp(self):
        super().setUp()
        key = self.root / "key"
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(key)], check=True)
        self.git("config", "gpg.format", "ssh")
        self.git("config", "user.signingkey", str(key) + ".pub")
        allowed_signers = self.root / "allowed_signers"
        allowed_signers.write_text(f"test@example.invalid {key.with_suffix('.pub').read_text()}")
        self.git("config", "gpg.ssh.allowedSignersFile", str(allowed_signers))
        self.git("push", "--quiet", "origin", f"{self.patch}:refs/tags/rehearsal/v0.2.1")
        self.state.write_text(json.dumps({"rehearsal/v0.2.1": {"isDraft": True}}))

    def green(self, **override):
        def runs(conclusion="success", title=None, sha=None, branch=None):
            return [{"headSha": sha or self.patch, "headBranch": branch, "status": "completed",
                     "conclusion": conclusion, "displayTitle": title or "", "url": "https://example.invalid/run"}]
        state = {"build.yml": runs(branch="rehearsal/v0.2.1"),
                 "gpu-amd.yml": runs(title="GPU CI - AMD (manual)"),
                 "gpu-nvidia.yml": runs(title="GPU CI - NVIDIA (manual)")}
        for workflow, value in override.items():
            state[workflow.replace("_", "-") + ".yml"] = value(runs)
        self.runs.write_text(json.dumps(state))

    def test_status_is_green_with_every_check(self):
        self.green()
        result = self.release("status", "0.2.1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("v0.2.1 can be tagged", result.stdout)

    def test_each_missing_check_blocks(self):
        cases = {
            "failed pipeline": dict(build=lambda r: r(conclusion="failure", branch="rehearsal/v0.2.1")),
            "pipeline on another commit": dict(build=lambda r: r(sha="0" * 40, branch="rehearsal/v0.2.1")),
            "filtered AMD suite": dict(gpu_amd=lambda r: r(title="GPU CI - AMD (manual) - only functional-shell")),
            "no NVIDIA run": dict(gpu_nvidia=lambda r: []),
            "NVIDIA still running": dict(gpu_nvidia=lambda r: [dict(r(title="GPU CI - NVIDIA (manual)")[0],
                                                                    status="in_progress", conclusion="")]),
        }
        for name, override in cases.items():
            with self.subTest(case=name):
                self.green(**override)
                result = self.release("status", "0.2.1")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("FAIL", result.stdout)

    def test_a_missing_draft_blocks(self):
        self.green()
        self.state.write_text("{}")
        self.assertNotEqual(self.release("status", "0.2.1").returncode, 0)

    def test_a_retry_after_a_success_still_counts(self):
        self.green(gpu_amd=lambda r: [dict(r()[0], status="in_progress", conclusion="", displayTitle="GPU CI - AMD (manual)"),
                                      dict(r()[0], displayTitle="GPU CI - AMD (manual)")])
        self.assertEqual(self.release("status", "0.2.1").returncode, 0)

    def test_run_limit_applies_after_filtering_by_commit(self):
        self.green(gpu_amd=lambda runs: (
            runs(title="GPU CI - AMD (manual)", sha="0" * 40) * 100
            + runs(title="GPU CI - AMD (manual)")
        ))
        result = self.release("status", "0.2.1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for call in self.gh_calls("run", "list"):
            self.assertEqual(call[call.index("--commit") + 1], self.patch)

    def test_without_a_rehearsal_there_is_no_status(self):
        self.git("push", "--quiet", "origin", ":refs/tags/rehearsal/v0.2.1")
        self.assertIn("rehearse 0.2.1 first", self.release("status", "0.2.1").stderr)

    def test_tag_signs_pushes_and_removes_the_rehearsal(self):
        self.green()
        result = self.release("tag", "0.2.1", "--yes")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.remote_tags(), {"v0.2.1": self.patch})
        self.assertIn("-----BEGIN SSH SIGNATURE-----", self.git("cat-file", "tag", "v0.2.1"))
        self.assertEqual(self.releases(), {})

    def test_tag_needs_a_green_status(self):
        self.green(gpu_nvidia=lambda r: [])
        result = self.release("tag", "0.2.1", "--yes")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("v0.2.1", self.remote_tags())
        self.assertEqual(self.git("tag", "--list", "v0.2.1"), "")

    def test_tag_asks_for_the_exact_name(self):
        self.green()
        result = subprocess.run([sys.executable, "-I", str(SCRIPT), "tag", "0.2.1"], cwd=self.work, env=self.env,
                                input="yes\n", capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not confirmed", result.stderr)
        self.assertNotIn("v0.2.1", self.remote_tags())
        result = subprocess.run([sys.executable, "-I", str(SCRIPT), "tag", "0.2.1"], cwd=self.work, env=self.env,
                                input="v0.2.1\n", capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("v0.2.1", self.remote_tags())

    def test_unsigned_tags_are_never_pushed(self):
        self.green()
        self.git("config", "--unset", "user.signingkey")
        self.git("config", "gpg.format", "openpgp")
        self.git("config", "gpg.program", "false")
        self.assertNotEqual(self.release("tag", "0.2.1", "--yes").returncode, 0)
        self.assertNotIn("v0.2.1", self.remote_tags())

    def test_an_unsigned_local_tag_on_the_commit_is_not_pushed(self):
        self.green()
        self.git("tag", "-a", "-m", "unsigned", "v0.2.1", self.patch)
        result = self.release("tag", "0.2.1", "--yes")
        self.assertIn("has no valid signature", result.stderr)
        self.assertNotIn("v0.2.1", self.remote_tags())

    def test_a_verified_local_tag_is_reused(self):
        self.green()
        self.git("tag", "-sm", "local release", "v0.2.1", self.patch)
        local = self.git("rev-parse", "refs/tags/v0.2.1")
        result = self.release("tag", "0.2.1", "--yes")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git("ls-remote", "--refs", "origin", "refs/tags/v0.2.1").split()[0], local)

    def test_a_local_tag_with_an_untrusted_signature_is_not_pushed(self):
        self.green()
        self.git("tag", "-sm", "local release", "v0.2.1", self.patch)
        self.git("config", "--unset", "gpg.ssh.allowedSignersFile")
        result = self.release("tag", "0.2.1", "--yes")
        self.assertIn("has no valid signature", result.stderr)
        self.assertNotIn("v0.2.1", self.remote_tags())

    def test_a_local_tag_on_another_commit_stops(self):
        self.green()
        self.git("tag", "v0.2.1", self.old_patch)
        result = self.release("tag", "0.2.1", "--yes")
        self.assertIn("delete it first", result.stderr)
        self.assertNotIn("v0.2.1", self.remote_tags())



class Finish(Fixture):
    ASSETS = [{"name": name} for name in ("SHA256SUMS", "SHA256SUMS-fedora", "SHA256SUMS-fedora-45",
                                          "SHA256SUMS-ubuntu", "vela_0.2.1_amd64.deb")]

    def publish(self, version, sha, **override):
        self.git("push", "--quiet", "origin", f"{sha}:refs/tags/v{version}")
        self.state.write_text(json.dumps({f"v{version}": {"isDraft": False, "assets": self.ASSETS, **override}}))

    def head(self, branch):
        return self.git("ls-remote", "origin", f"refs/heads/{branch}").split()[0]

    def pr(self, head):
        return next(c for c in self.gh_calls("pr", "create") if c[c.index("--head") + 1] == head)

    def test_patch_release_moves_manual_builds_on_both_branches(self):
        self.publish("0.2.1", self.patch)
        result = self.release("finish", "0.2.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        for base, head in (("release/0.2", "finish/v0.2.1-release-0.2"), ("main", "finish/v0.2.1-main")):
            with self.subTest(base=base):
                commit = self.head(head)
                self.assertEqual(self.git("diff", "--name-only", f"{commit}^", commit), "packaging/arch/PKGBUILD.beta")
                self.assertIn("pkgver=${VELA_RELEASE_VERSION:-0.2.1}",
                              self.git("show", f"{commit}:packaging/arch/PKGBUILD.beta"))
                self.assertIn("Signed-off-by: Test", self.git("log", "-1", "--format=%B", commit))
                pr = self.pr(head)
                self.assertEqual(pr[pr.index("--base") + 1], base)
        self.assertEqual(self.git("rev-parse", f"{self.head('finish/v0.2.1-main')}^"),
                         self.main_commit)
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_minor_release_creates_its_branch_and_moves_main_on(self):
        self.git("push", "--quiet", "origin", ":refs/heads/release/0.2")
        self.publish("0.2.0", self.main_commit)
        result = self.release("finish", "0.2.0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.head("release/0.2"), self.main_commit)
        branch = self.head("finish/v0.2.0-release-0.2")
        self.assertEqual(self.git("diff", "--name-only", self.main_commit, branch), "packaging/arch/PKGBUILD.beta")
        main = self.head("finish/v0.2.0-main")
        self.assertEqual(sorted(self.git("diff", "--name-only", self.main_commit, main).split()),
                         ["CMakeLists.txt", "packaging/arch/PKGBUILD.beta"])
        self.assertIn("VERSION 0.3.0", self.git("show", f"{main}:CMakeLists.txt"))
        pr = self.pr("finish/v0.2.0-main")
        self.assertIn("start 0.3.0", pr[pr.index("--title") + 1])

    def test_finish_reuses_the_pushed_branch_and_then_the_open_pr(self):
        self.publish("0.2.1", self.patch)
        head = "finish/v0.2.1-release-0.2"
        self.env["TEST_GH_FAIL_CREATE"] = head
        first = self.release("finish", "0.2.1")
        self.assertNotEqual(first.returncode, 0)
        self.assertIn("could not create pull request", first.stderr)
        pushed = self.head(head)
        del self.env["TEST_GH_FAIL_CREATE"]

        # An unrelated change to the base must not require rewriting the head.
        self.git("switch", "--quiet", "release/0.2")
        self.commit("0.2.1", "advance the base")
        self.git("push", "--quiet", "origin", "release/0.2")
        retry = self.release("finish", "0.2.1")
        self.assertEqual(retry.returncode, 0, retry.stderr)
        self.assertEqual(self.head(head), pushed)
        self.pr("finish/v0.2.1-main")

        creates = self.gh_calls("pr", "create")
        again = self.release("finish", "0.2.1")
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual(again.stdout.count("Already open:"), 2)
        self.assertEqual(self.gh_calls("pr", "create"), creates)

    def test_finish_does_not_reuse_an_unrelated_remote_head(self):
        self.publish("0.2.1", self.patch)
        head = "finish/v0.2.1-release-0.2"
        other = self.commit("0.2.1", "unexpected change")
        self.git("push", "--quiet", "origin", f"{other}:refs/heads/{head}")
        result = self.release("finish", "0.2.1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not contain the intended change", result.stderr)
        self.assertEqual(self.head(head), other)
        self.assertEqual(self.gh_calls("pr", "create"), [])

    def test_finish_keeps_equal_and_newer_fallbacks(self):
        self.publish("0.2.2", self.patch)
        for base, fallback in (("release/0.2", "0.2.10"), ("main", "0.2.2")):
            self.git("switch", "--quiet", base)
            path = self.work / release.PKGBUILD_BETA
            path.write_text(path.read_text().replace("0.1.2", fallback))
            self.git("commit", "--quiet", "-am", "set existing fallback")
            self.git("push", "--quiet", "origin", base)

        result = self.release("finish", "0.2.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count("needs no change"), 2)
        self.assertEqual(self.gh_calls("pr", "create"), [])
        self.assertEqual(self.git("ls-remote", "origin", "refs/heads/finish/*"), "")

    def test_only_a_published_release_with_its_manifests_is_finished(self):
        cases = {"not published": None, "draft": dict(isDraft=True),
                 "missing manifest": dict(assets=self.ASSETS[1:])}
        for name, override in cases.items():
            with self.subTest(case=name):
                if override is None:
                    self.state.write_text("{}")
                else:
                    self.publish("0.2.1", self.patch, **override)
                result = self.release("finish", "0.2.1")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.gh_calls("pr", "create"), [])
                self.git("push", "--quiet", "origin", ":refs/tags/v0.2.1")

    def test_a_patch_without_its_release_branch_stops(self):
        self.git("push", "--quiet", "origin", ":refs/heads/release/0.2")
        self.publish("0.2.1", self.patch)
        result = self.release("finish", "0.2.1")
        self.assertIn("is a patch release", result.stderr)
        self.assertNotIn("release/0.2", self.git("ls-remote", "--heads", "origin"))


if __name__ == "__main__":
    unittest.main()
