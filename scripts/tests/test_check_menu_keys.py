# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("menukeys", Path(__file__).parents[1] / "check-menu-keys.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

MENUS = """
    const fileEntry = (file, pinned) => ({
        key: pinned ? "taskbar.pinned" : "taskbar.recent",
        text: file.name })
    { key: "file.copy", icon: "edit-copy", text: qsTr("Copy") },
    item("winx.files", qsTr("File &Explorer"), "system-file-manager-symbolic", "files"),
    { icon: "zoom-in", text: qsTr("Magnifier"), key: "magnifier" },
"""
PAGE = """
    { keys: ["taskbar.pinned"], text: qsTr("Pinned files") },
    { keys: ["taskbar.recent"], text: qsTr("Recent files") },
    { keys: ["file.copy"], text: qsTr("Copy") },
    { keys: ["winx.files"], text: qsTr("File Explorer") },
"""


class MenuKeys(unittest.TestCase):
    def test_the_repository_agrees(self):
        self.assertEqual(m.main(), 0)

    def test_the_same_keys_pass(self):
        self.assertEqual(m.check(MENUS, PAGE), [])

    def test_an_entry_without_a_switch_fails(self):
        self.assertEqual(m.check(MENUS + '{ key: "file.open", text: qsTr("Open") }', PAGE),
                         ["file.open: in a menu, not in Settings"])

    def test_a_switch_hiding_nothing_fails(self):
        self.assertEqual(m.check(MENUS, PAGE + '{ keys: ["desktop.view"], text: qsTr("View") }'),
                         ["desktop.view: in Settings, in no menu"])

    def test_a_key_with_digits_is_seen(self):
        self.assertEqual(m.check(MENUS + '{ key: "file.openWith2", text: qsTr("Open with") }', PAGE),
                         ["file.openWith2: in a menu, not in Settings"])

    def test_a_commented_out_entry_counts_on_neither_side(self):
        menus = MENUS.replace('    item("winx.files"', '    // item("winx.files"')
        self.assertEqual(m.check(menus, PAGE), ["winx.files: in Settings, in no menu"])
        page = PAGE + '/* { keys: ["desktop.view"], text: qsTr("View") } */'
        self.assertEqual(m.check(MENUS, page), [])

    def test_comment_marks_inside_strings_stay(self):
        self.assertEqual(m.strip_comments('a: "http://x" // gone\nb'), 'a: "http://x" \nb')

    def test_keys_of_other_lists_are_ignored(self):
        self.assertNotIn("magnifier", m.menu_keys(MENUS))


if __name__ == "__main__":
    unittest.main()
