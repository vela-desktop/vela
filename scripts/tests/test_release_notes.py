#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Release notes come from one CHANGELOG.md section, as plain text."""

import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/release-notes.py"
spec = importlib.util.spec_from_file_location("release_notes", SCRIPT)
notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notes)

CHANGELOG = """# Changelog

## Unreleased

- Not yet.

## 0.2.1 — 3 December 2026

- The taskbar keeps its edge ([taskbar](docs/taskbar.md#7-super19-and-super0)).
- See [#310](https://github.com/vela-desktop/vela/pull/310) and
  <https://example.invalid>.

## 0.2.0 — 1 December 2026

- Themes.
"""


class Notes(unittest.TestCase):
    def test_only_the_versions_section(self):
        text = notes.notes(CHANGELOG, "0.2.0")
        self.assertTrue(text.startswith("- Themes.\n\nInstall or update: "))
        self.assertNotIn("taskbar", text)

    def test_links_become_plain_text_pinned_to_the_tag(self):
        text = notes.notes(CHANGELOG, "0.2.1")
        self.assertIn("(taskbar (https://github.com/vela-desktop/vela/blob/v0.2.1/docs/taskbar.md#7-super19-and-super0))",
                      text)
        self.assertIn("#310 (https://github.com/vela-desktop/vela/pull/310)", text)
        self.assertNotIn("](", text)
        self.assertNotIn("Not yet", text)

    def test_footer_points_at_the_tag(self):
        text = notes.notes(CHANGELOG, "0.2.1")
        self.assertTrue(text.endswith(
            "Install or update: https://github.com/vela-desktop/vela/blob/v0.2.1/docs/installation.md\n"
            "Changelog: https://github.com/vela-desktop/vela/blob/v0.2.1/CHANGELOG.md\n"))

    def test_a_link_whose_label_is_its_url(self):
        self.assertEqual(notes.plain("[https://a.invalid](https://a.invalid)", "0.2.1"), "https://a.invalid")

    def test_missing_or_empty_sections_stop_the_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scripts = root / "scripts"
            scripts.mkdir()
            script = Path(shutil.copyfile(SCRIPT, scripts / SCRIPT.name))
            for text, version, error in (
                (CHANGELOG, "0.2.2", "no CHANGELOG.md entries"),
                ("## 0.2.2 — 3 December 2026\n\n## 0.2.1\n", "0.2.2", "no CHANGELOG.md entries"),
                (CHANGELOG, "0.2", "not a Beta version"),
            ):
                with self.subTest(version=version, text=text[:20]):
                    (root / "CHANGELOG.md").write_text(text, encoding="utf-8")
                    result = subprocess.run([sys.executable, "-I", str(script), version],
                                            cwd=scripts, capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, "")
                    self.assertIn(error, result.stderr)

    def test_cli_reads_the_repository_changelog_from_another_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, "-I", str(SCRIPT), "0.1.2"],
                                    cwd=directory, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertEqual(result.stdout, notes.notes(text, "0.1.2"))

    def test_cli_refuses_a_changelog_override(self):
        result = subprocess.run([sys.executable, "-I", str(SCRIPT), "0.1.2", "--changelog", "CHANGELOG.md"],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("unrecognized arguments: --changelog", result.stderr)

    def test_a_version_prefix_is_not_its_section(self):
        # 0.2.1 must not pick up a 0.2.10 section.
        with self.assertRaises(SystemExit):
            notes.section("## 0.2.10 — 1 January 2027\n\n- Later.\n", "0.2.1")

    def test_the_real_changelog_has_every_published_section(self):
        text = (ROOT / "CHANGELOG.md").read_text()
        for version in ("0.1.0", "0.1.1", "0.1.2"):
            with self.subTest(version=version):
                self.assertTrue(notes.section(text, version))


if __name__ == "__main__":
    unittest.main()
