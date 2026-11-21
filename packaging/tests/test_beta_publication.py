#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Exercise the actual publication shell with local packages and a fake GitHub CLI."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
GH_STUB = r'''#!/usr/bin/env python3
import json, os, pathlib, shutil, sys
args = sys.argv[1:]
with open(os.environ["TEST_GH_LOG"], "a") as log:
    log.write(json.dumps(args) + "\n")
if args[:2] == ["attestation", "verify"]:
    sys.exit(0)
if args[:2] == ["release", "view"]:
    if not os.environ.get("TEST_EXISTING"):
        sys.exit(1)
    if "--json" in args:
        field = args[args.index("--json") + 1]
        if field == "isDraft":
            print(os.environ.get("TEST_DRAFT", "false"))
        elif field == "assets":
            print("\n".join(p.name for p in pathlib.Path.cwd().iterdir() if p.is_file()))
        else:
            sys.exit("Unexpected release metadata: " + field)
elif args[:2] == ["release", "download"]:
    dest = pathlib.Path(args[args.index("--dir") + 1])
    for manifest in pathlib.Path.cwd().glob("SHA256SUMS*"):
        shutil.copyfile(manifest, dest / manifest.name)
    if os.environ.get("TEST_CHANGED_MANIFEST"):
        (dest / "SHA256SUMS").write_text("changed bytes\n")
elif args[:2] != ["release", "create"]:
    sys.exit("Unexpected gh call: " + repr(args))
'''


class BetaPublication(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="vela-beta-publication-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        dist = self.root / "dist"
        dist.mkdir()
        for name in ("vela-0.1.3-1-x86_64.pkg.tar.zst",
                     "vela-0.1.3-1.fc44.x86_64.rpm",
                     "vela-0.1.3-1.fc45.x86_64.rpm",
                     "vela_0.1.3_amd64.deb"):
            (dist / name).write_bytes(b"package fixture\n")
        # Written by the previous step from CHANGELOG.md.
        (self.root / "release-notes.txt").write_text("- A fix.\n\nInstall or update: https://example.invalid\n")
        bindir = self.root / "bin"
        bindir.mkdir()
        gh = bindir / "gh"
        gh.write_text(GH_STUB)
        gh.chmod(0o755)
        self.log = self.root / "gh.jsonl"
        self.env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}",
                    "TEST_GH_LOG": str(self.log), "GITHUB_REF_NAME": "v0.1.3",
                    "GITHUB_REF": "refs/tags/v0.1.3", "GITHUB_SHA": "a" * 40,
                    "GITHUB_REPOSITORY": "vela-desktop/vela"}
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        step = workflow.split("- name: Publish signed Beta release\n", 1)[1]
        # The step runs until the next job.
        self.script = textwrap.dedent(step.split("        run: |\n", 1)[1].split("\n\n  ", 1)[0])

    def publish(self):
        return subprocess.run(["bash", "-c", self.script], cwd=self.root,
                              env=self.env, capture_output=True, text=True, timeout=20)

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_new_beta_is_a_normal_release_with_beta_title(self):
        result = self.publish()
        self.assertEqual(result.returncode, 0, result.stderr)
        created = [cmd for cmd in self.commands() if cmd[:2] == ["release", "create"]]
        self.assertEqual(len(created), 1)
        self.assertIn("--prerelease=false", created[0])
        self.assertIn("--verify-tag", created[0])
        self.assertEqual(created[0][created[0].index("--title") + 1], "Vela 0.1.3 Beta")
        # Notes come from CHANGELOG.md, not GitHub's generated changelog link.
        self.assertNotIn("--generate-notes", created[0])
        self.assertEqual(created[0][created[0].index("--notes-file") + 1], "../release-notes.txt")

    def test_rerun_keeps_existing_published_release_unchanged(self):
        # No prerelease metadata is consulted: both old and reclassified
        # releases retain their assets and hand-written notes.
        self.env["TEST_EXISTING"] = "1"
        result = self.publish()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("keeping it unchanged", result.stdout)
        self.assertFalse(any(cmd[:2] in (["release", "create"], ["release", "edit"])
                             for cmd in self.commands()))

    def test_rerun_rejects_drafts(self):
        self.env.update(TEST_EXISTING="1", TEST_DRAFT="true")
        self.assertNotEqual(self.publish().returncode, 0)

    def test_rerun_still_rejects_changed_package_manifests(self):
        self.env.update(TEST_EXISTING="1", TEST_CHANGED_MANIFEST="1")
        self.assertNotEqual(self.publish().returncode, 0)

    def rehearse(self, tag="rehearsal/v0.1.3"):
        self.env.update(GITHUB_REF_NAME=tag, GITHUB_REF="refs/tags/" + tag)

    def test_rehearsal_creates_a_draft_with_the_same_assets(self):
        self.rehearse()
        result = self.publish()
        self.assertEqual(result.returncode, 0, result.stderr)
        created = [cmd for cmd in self.commands() if cmd[:2] == ["release", "create"]]
        self.assertEqual(len(created), 1)
        self.assertEqual(created[0][2], "rehearsal/v0.1.3")
        self.assertIn("--draft", created[0])
        self.assertIn("--verify-tag", created[0])
        self.assertNotIn("--generate-notes", created[0])
        self.assertEqual(created[0][created[0].index("--title") + 1], "Rehearsal of Vela 0.1.3 Beta")
        notes = created[0][created[0].index("--notes-file") + 1]
        self.assertEqual(notes, "../rehearsal-notes.txt")
        text = (self.root / "rehearsal-notes.txt").read_text()
        self.assertTrue(text.startswith("Rehearsal of v0.1.3 on " + "a" * 40 + ". Not a release"))
        self.assertTrue(text.endswith("- A fix.\n\nInstall or update: https://example.invalid\n"))
        for asset in ("vela-0.1.3-1-x86_64.pkg.tar.zst", "vela-0.1.3-1.fc44.x86_64.rpm",
                      "vela-0.1.3-1.fc45.x86_64.rpm", "vela_0.1.3_amd64.deb",
                      "SHA256SUMS", "SHA256SUMS-fedora", "SHA256SUMS-fedora-45", "SHA256SUMS-ubuntu"):
            self.assertIn(asset, created[0])

    def test_real_release_is_never_a_draft(self):
        self.assertEqual(self.publish().returncode, 0)
        created = [cmd for cmd in self.commands() if cmd[:2] == ["release", "create"]]
        self.assertNotIn("--draft", created[0])

    def test_rehearsal_attestations_name_the_rehearsal_tag(self):
        self.rehearse()
        self.assertEqual(self.publish().returncode, 0)
        verified = [cmd for cmd in self.commands() if cmd[:2] == ["attestation", "verify"]]
        self.assertEqual(len(verified), 4)
        for cmd in verified:
            self.assertEqual(cmd[cmd.index("--source-ref") + 1], "refs/tags/rehearsal/v0.1.3")

    def test_rehearsal_rerun_keeps_its_draft(self):
        self.rehearse()
        self.env.update(TEST_EXISTING="1", TEST_DRAFT="true")
        result = self.publish()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("keeping it unchanged", result.stdout)

    def test_rehearsal_rerun_rejects_a_published_release(self):
        # A rehearsal tag must never reuse a release that users can see.
        self.rehearse()
        self.env.update(TEST_EXISTING="1", TEST_DRAFT="false")
        self.assertNotEqual(self.publish().returncode, 0)

    def test_rehearsal_rerun_rejects_changed_manifests(self):
        self.rehearse()
        self.env.update(TEST_EXISTING="1", TEST_DRAFT="true", TEST_CHANGED_MANIFEST="1")
        self.assertNotEqual(self.publish().returncode, 0)

    def test_malformed_tags_never_publish(self):
        for tag in ("rehearsal/v0.1.3a", "rehearsal/0.1.3", "v0.1.3-rc1", "other/v0.1.3"):
            with self.subTest(tag=tag):
                self.rehearse(tag)
                self.assertNotEqual(self.publish().returncode, 0)


if __name__ == "__main__":
    unittest.main()
