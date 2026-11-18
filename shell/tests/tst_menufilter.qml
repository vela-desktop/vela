// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import "../qml/menufilter.js" as MenuFilter

// The entries a menu shows once the user hid some (menufilter.js): hidden
// keys go with their submenus, and nothing is left dangling.
TestCase {
    name: "MenuFilter"

    function texts(entries) {
        return entries.map(e => e.separator ? "|" : e.header !== undefined ? "#" + e.header : e.text)
    }

    function test_nothing_hidden_keeps_everything() {
        const entries = [{ key: "a", text: "A" }, { separator: true }, { text: "B" }]
        compare(texts(MenuFilter.visible(entries, [])), ["A", "|", "B"])
        compare(texts(MenuFilter.visible(entries, undefined)), ["A", "|", "B"])
    }

    function test_hidden_keys_go_and_keyless_entries_stay() {
        const entries = [{ key: "a", text: "A" }, { key: "b", text: "B" }, { text: "C" }]
        compare(texts(MenuFilter.visible(entries, ["b", "c"])), ["A", "C"])
    }

    function test_separators_left_alone_go() {
        const entries = [
            { key: "a", text: "A" }, { separator: true },
            { key: "b", text: "B" }, { separator: true },
            { key: "c", text: "C" }, { separator: true },
            { key: "d", text: "D" }
        ]
        compare(texts(MenuFilter.visible(entries, ["a"])), ["B", "|", "C", "|", "D"], "none first")
        compare(texts(MenuFilter.visible(entries, ["b"])), ["A", "|", "C", "|", "D"], "none doubled")
        compare(texts(MenuFilter.visible(entries, ["d"])), ["A", "|", "B", "|", "C"], "none last")
        compare(texts(MenuFilter.visible(entries, ["a", "b", "c", "d"])), [])
    }

    function test_a_group_goes_with_its_title() {
        const entries = [
            { key: "pinned", header: "Pinned" }, { key: "pinned", text: "a.txt" },
            { key: "recent", header: "Recent" }, { key: "recent", text: "b.txt" },
            { separator: true }, { text: "App" }
        ]
        compare(texts(MenuFilter.visible(entries, ["pinned"])), ["#Recent", "b.txt", "|", "App"])
        compare(texts(MenuFilter.visible(entries, ["pinned", "recent"])), ["App"])
    }

    function test_a_title_without_entries_goes() {
        const entries = [{ header: "Tasks" }, { key: "t", text: "New window" }, { separator: true }, { text: "App" }]
        compare(texts(MenuFilter.visible(entries, ["t"])), ["App"])
    }

    function test_submenus_are_filtered_and_dropped_when_empty() {
        const action = () => {}
        const entries = [
            { text: "Ark", children: [{ key: "s/x", text: "Extract", action: action }, { key: "s/c", text: "Compress" }] },
            { key: "p", text: "Properties" }
        ]
        const one = MenuFilter.visible(entries, ["s/c"])
        compare(texts(one), ["Ark", "Properties"])
        compare(texts(one[0].children), ["Extract"])
        verify(one[0].children[0].action === action, "the entry itself, its action with it")
        compare(entries[0].children.length, 2, "the menu's own entries aren't changed")
        compare(texts(MenuFilter.visible(entries, ["s/x", "s/c"])), ["Properties"])
    }

    function test_icon_row_keeps_its_other_buttons() {
        const row = { iconRow: [{ key: "cut", icon: "edit-cut" }, { key: "copy", icon: "edit-copy" }] }
        const entries = [row, { text: "Open" }]
        const shown = MenuFilter.visible(entries, ["cut"])
        compare(shown[0].iconRow.map(b => b.key), ["copy"])
        compare(row.iconRow.length, 2)
        verify(MenuFilter.visible(entries, [])[0] === row)
        compare(texts(MenuFilter.visible(entries, ["cut", "copy"])), ["Open"], "an empty row goes")
    }

    function test_icon_rows_become_rows() {
        const cut = { key: "cut", icon: "edit-cut", text: "Cut", action: () => {} }
        const rows = MenuFilter.iconRowsAsRows([{ iconRow: [cut, { key: "copy", text: "Copy" }] }, { text: "Open" }])
        compare(texts(rows), ["Cut", "Copy", "|", "Open"])
        verify(rows[0].action === cut.action)
        compare(texts(MenuFilter.iconRowsAsRows([{ iconRow: [cut] }])), ["Cut"], "no separator at the end")
    }
}
