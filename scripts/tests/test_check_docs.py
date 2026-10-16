"""Regression tests for source-build dependency documentation parsing."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


CHECKER = Path(__file__).resolve().parents[1] / "check-docs.py"
spec = importlib.util.spec_from_file_location("vela_check_docs", CHECKER)
assert spec and spec.loader
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class BuildDependencyParserTests(unittest.TestCase):
    def test_ignores_headings_and_short_bootstrap_block(self):
        docs = (
            "## A renamed user heading\n"
            "```sh\nsudo dnf install git curl gh\n```\n"
            "## A completely different developer heading\n"
            "```bash\nsudo dnf install git cmake \\\n"
            "    wlroots-devel qt6-qtbase-devel\n```\n"
        )
        self.assertEqual(
            checker.documented_build_dependencies(docs, "dnf install", "wlroots-devel"),
            {"git", "cmake", "wlroots-devel", "qt6-qtbase-devel"},
        )

    def test_arch_block_can_use_tilde_fence(self):
        docs = "~~~shell\nsudo pacman -S --needed git wlroots0.20 wayland\n~~~\n"
        self.assertEqual(
            checker.documented_build_dependencies(docs, "pacman -S --needed", "wlroots0.20"),
            {"git", "wlroots0.20", "wayland"},
        )

    def test_four_backtick_fence(self):
        docs = "````sh\nsudo dnf install git wlroots-devel\n````\n"
        self.assertEqual(
            checker.documented_build_dependencies(docs, "dnf install", "wlroots-devel"),
            {"git", "wlroots-devel"},
        )

    def test_longer_tilde_fence_and_longer_closer(self):
        docs = "~~~~shell\nsudo dnf install git wlroots-devel\n~~~~~~\n"
        self.assertEqual(
            checker.documented_build_dependencies(docs, "dnf install", "wlroots-devel"),
            {"git", "wlroots-devel"},
        )

    def test_shorter_and_wrong_character_fences_do_not_close(self):
        docs = (
            "`````sh\n```\n~~~\n"
            "sudo dnf install git wlroots-devel\n``````\n"
        )
        self.assertEqual(
            checker.documented_build_dependencies(docs, "dnf install", "wlroots-devel"),
            {"git", "wlroots-devel"},
        )

    def test_shell_example_inside_non_shell_fence_is_skipped(self):
        docs = (
            "````python\n```sh\nsudo dnf install bogus wlroots-devel\n"
            "```\n````\n"
            "```sh\nsudo dnf install git wlroots-devel\n```\n"
        )
        self.assertEqual(
            checker.documented_build_dependencies(docs, "dnf install", "wlroots-devel"),
            {"git", "wlroots-devel"},
        )

    def test_unclosed_long_fence_is_ignored(self):
        docs = "````sh\nsudo dnf install wlroots-devel\n```\n"
        with self.assertRaisesRegex(ValueError, "found 0"):
            checker.documented_build_dependencies(docs, "dnf install", "wlroots-devel")

    def test_missing_full_block_has_clear_error(self):
        docs = "```sh\nsudo dnf install git curl gh\n```\n"
        with self.assertRaisesRegex(ValueError, "expected one full source-build dependency block"):
            checker.documented_build_dependencies(docs, "dnf install", "wlroots-devel")

    def test_duplicate_full_blocks_are_rejected(self):
        docs = "```sh\nsudo dnf install wlroots-devel\n```\n" * 2
        with self.assertRaisesRegex(ValueError, "found 2"):
            checker.documented_build_dependencies(docs, "dnf install", "wlroots-devel")


if __name__ == "__main__":
    unittest.main()
