#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline retention tests. All GitHub API operations are simulated."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PRUNE = ROOT / "scripts/prune-alpha-releases.sh"
REPO = "vela-desktop/vela"
CURRENT = f"{9:040x}"

GH_STUB = r'''#!/usr/bin/env python3
import json, os, pathlib, re, sys

args = sys.argv[1:]
log = pathlib.Path(os.environ["TEST_GH_LOG"])
with log.open("a") as f:
    f.write(json.dumps(args) + "\n")

if args[:1] == ["api"]:
    if os.environ.get("TEST_FAIL_API"):
        sys.exit(1)
    assert args[1] == "--paginate"
    assert args[2] == "repos/vela-desktop/vela/releases?per_page=100"
    assert args[3] == "--jq"
    query = args[4]
    for guard in (".prerelease == true", ".draft == false",
                  'test("^alpha-[0-9a-f]{40}$")',
                  'startswith("Vela alpha ")',
                  ".published_at != null"):
        assert guard in query, "Missing release filter: " + guard
    # Simulate the JSON filter applied by gh api --jq.
    for r in json.loads(os.environ["TEST_RELEASES"]):
        if (r["prerelease"] and not r["draft"]
            and re.fullmatch(r"alpha-[0-9a-f]{40}", r["tag_name"])
            and r["name"].startswith("Vela alpha ")
            and r["published_at"]):
            print(r["published_at"] + "\t" + r["tag_name"])
elif args[:2] == ["release", "delete"]:
    assert args[3:] == ["-R", "vela-desktop/vela", "--yes", "--cleanup-tag"], args
    if os.environ.get("TEST_FAIL_DELETE"):
        sys.exit(1)
else:
    sys.exit("Unexpected gh command: " + repr(args))
'''


def alpha(number, **overrides):
    commit = f"{number:040x}"
    obj = {
        "tag_name": "alpha-" + commit,
        "name": "Vela alpha " + commit[:12],
        "prerelease": True,
        "draft": False,
        "published_at": f"2026-10-{number + 1:02d}T12:00:00Z",
    }
    obj.update(overrides)
    return obj


class ReleaseRetention(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="vela-release-retention-")
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        stub = bin_dir / "gh"
        stub.write_text(GH_STUB)
        stub.chmod(0o755)
        self.log = self.dir / "gh.jsonl"
        self.env = os.environ.copy()
        self.env.update({
            "PATH": f"{bin_dir}:{self.env['PATH']}",
            "GITHUB_REPOSITORY": REPO,
            "GITHUB_SHA": CURRENT,
            "TEST_GH_LOG": str(self.log),
        })

    def run_prune(self, releases, *, succeed=True, **extra):
        env = {**self.env, "TEST_RELEASES": json.dumps(releases), **extra}
        result = subprocess.run(
            ["sh", str(PRUNE)], env=env, capture_output=True, text=True, timeout=15,
        )
        self.assertEqual(result.returncode == 0, succeed, result.stdout + result.stderr)
        return result

    def deleted(self):
        if not self.log.exists():
            return []
        commands = [json.loads(line) for line in self.log.read_text().splitlines()]
        return [command[2] for command in commands if command[:2] == ["release", "delete"]]

    def test_keep_five_most_recent_from_unsorted_listing(self):
        items = [alpha(i) for i in (4, 9, 1, 8, 3, 6, 0, 7, 2, 5)]
        self.run_prune(items)
        self.assertEqual(set(self.deleted()), {f"alpha-{i:040x}" for i in range(5)})
        self.assertNotIn("alpha-" + CURRENT, self.deleted())

    def test_no_deletions_when_five_or_fewer(self):
        self.run_prune([alpha(i) for i in range(5, 10)])
        self.assertEqual(self.deleted(), [])

    def test_never_delete_other_release_types(self):
        releases = [alpha(i) for i in range(10)]
        releases += [
            alpha(0, tag_name="v1.0.0", name="Vela stable", prerelease=False),
            alpha(1, name="Manually published test alpha"),
            alpha(2, draft=True),
            alpha(3, tag_name="alpha-not-a-sha"),
            alpha(4, published_at=None),
        ]
        self.run_prune(releases)
        self.assertEqual(set(self.deleted()), {f"alpha-{i:040x}" for i in range(5)})

    def test_missing_current_release_aborts_before_deletion(self):
        result = self.run_prune([alpha(i) for i in range(9)], succeed=False)
        self.assertIn("not found; skipping cleanup", result.stderr)
        self.assertEqual(self.deleted(), [])

    def test_api_error_aborts_before_deletion(self):
        self.run_prune([alpha(i) for i in range(10)], succeed=False, TEST_FAIL_API="1")
        self.assertEqual(self.deleted(), [])

    def test_failed_delete_makes_job_fail(self):
        self.run_prune([alpha(i) for i in range(10)], succeed=False,
                       TEST_FAIL_DELETE="1")
        self.assertEqual(self.deleted()[:1], [f"alpha-{4:040x}"])

    def test_reject_invalid_commit_identifier(self):
        self.run_prune([alpha(i) for i in range(10)], succeed=False,
                       GITHUB_SHA="not-a-commit")
        self.assertEqual(self.deleted(), [])


if __name__ == "__main__":
    unittest.main()
