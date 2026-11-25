// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// One level of a menu (MenuPanel.qml) as the entries change under it: the
// icon row reachable from the keyboard, as wide as its buttons, no taller
// than the output with its rows scrolling, and the keyboard selection
// following its entry when Settings hides another.
Item {
    id: root
    width: 600
    height: 300

    readonly property var iconRow: ({ iconRow: [
        { key: "file.cut", icon: "edit-cut", text: "Cut" },
        { key: "file.copy", icon: "edit-copy", text: "Copy" },
        { key: "file.rename", icon: "edit-rename", text: "Rename" },
        { key: "file.share", icon: "document-share", text: "Share" },
        { key: "file.delete", icon: "edit-delete", text: "Delete" }
    ] })

    Component {
        id: panelComponent
        MenuPanel { anchorX: 10; anchorY: 10 }
    }

    TestCase {
        name: "MenuPanel"
        when: windowShown

        function make(properties) {
            return createTemporaryObject(panelComponent, root, properties)
        }
        function texts(panel) {
            return panel.visibleEntries.map(e => e.separator ? "|" : e.iconRow ? "[icons]" : e.text)
        }

        function test_icon_row_buttons_are_rows_from_the_keyboard() {
            const entries = [root.iconRow, { text: "Open" }]
            compare(texts(make({ entries: entries })), ["[icons]", "Open"])
            const keyboard = make({ entries: entries, keyboardMode: true })
            compare(texts(keyboard), ["Cut", "Copy", "Rename", "Share", "Delete", "|", "Open"])
            keyboard.move(1)
            compare(keyboard.currentIndex, 0, "the arrows reach Cut")
        }

        function test_the_icon_row_counts_in_the_width() {
            const panel = make({ entries: [root.iconRow] })
            verify(panel.width >= 8 + 5 * 36 + 4 * 4 + 8, "width " + panel.width)
        }

        function test_a_long_menu_scrolls_within_the_output() {
            const entries = []
            for (let i = 0; i < 20; ++i) {
                entries.push({ text: "Entry " + i })
            }
            const panel = make({ entries: entries, keyboardMode: true })
            verify(panel.contentHeight > root.height)
            compare(panel.height, root.height - 16)
            verify(panel.y >= 8 && panel.y + panel.height <= root.height - 8)
            const scroller = findChild(panel, "menuRows")
            verify(scroller.interactive)
            compare(scroller.contentY, 0)
            keyClick(Qt.Key_End)
            compare(panel.currentIndex, 19)
            verify(scroller.contentY > 0, "the last row scrolled into view")
            verify(panel.rowY(19) + panel.rowHeight <= scroller.contentY + scroller.height + panel.padding)
        }

        function test_a_short_menu_does_not_scroll() {
            const panel = make({ entries: [{ text: "A" }, { text: "B" }] })
            compare(panel.height, panel.contentHeight)
            verify(!findChild(panel, "menuRows").interactive)
        }

        function test_the_selection_follows_its_entry_when_the_rows_change() {
            const panel = make({ entries: [{ key: "a", text: "View" }, { key: "b", text: "Sort by" }, { key: "c", text: "Refresh" }] })
            const changed = signalSpy.createObject(root, { target: panel, signalName: "layoutChanged" })
            panel.currentIndex = 1
            panel.entries = [{ key: "b", text: "Sort by" }, { key: "c", text: "Refresh" }]
            compare(panel.currentIndex, 0, "still on Sort by")
            compare(changed.count, 1)
            panel.entries = [{ key: "c", text: "Refresh" }]
            compare(panel.currentIndex, -1, "Sort by gone: no selection, not Refresh")
            keyClick(Qt.Key_Right) // nothing selected: no submenu, no error
        }

        Component {
            id: signalSpy
            SignalSpy {}
        }
    }
}
