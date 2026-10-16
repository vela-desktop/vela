// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "colors.js" as Colors

// ListItem: subtle, or the control's colors when highlighted, by precedence
// and faded; Qt's keys and focus. And the small pieces the controls are made
// of: Icon, Chevron, FocusRing (docs/visual-coherence.md §6.8).
Item {
    id: root
    width: 400
    height: 300

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    ListItem { id: row; x: 20; y: 20; width: 200; height: 40; text: "Row" }
    ListItem { id: chosen; x: 20; y: 70; width: 200; height: 40; text: "Chosen"; highlighted: true }
    SignalSpy { id: clicks; target: row; signalName: "clicked" }
    Icon { id: icon; name: "go-previous"; size: 18 }
    Icon { id: none }
    Chevron { id: right }
    Chevron { id: down; down: true }

    TestCase {
        name: "ListItem"
        when: windowShown

        function init() {
            mouseMove(root, 390, 290)
            start.forceActiveFocus()
            clicks.clear()
            tryVerify(() => Colors.same(row.background.color, Theme.subtleRest), 2000)
            tryVerify(() => Colors.same(chosen.background.color, Theme.control), 2000)
        }

        function test_states_reach_their_colors() {
            for (const [item, rest, hover, pressed] of [[row, Theme.subtleRest, Theme.subtleHover, Theme.subtlePressed],
                                                       [chosen, Theme.control, Theme.controlHover, Theme.controlPressed]]) {
                mouseMove(item, 10, 10)
                tryVerify(() => Colors.same(item.background.color, hover), 2000, item.text + " hovered")
                mousePress(item, 10, 10)
                tryVerify(() => Colors.same(item.background.color, pressed), 2000, item.text + " pressed")
                mouseRelease(item, 10, 10)
                mouseMove(root, 390, 290)
                tryVerify(() => Colors.same(item.background.color, rest), 2000, item.text + " back at rest")
            }
        }

        function test_hover_fades() {
            mouseMove(row, 10, 10)
            wait(Theme.fast / 3)
            verify(Colors.between(row.background.color, Theme.subtleRest, Theme.subtleHover))
        }

        function test_click_space_and_enter_activate() {
            mouseClick(row)
            compare(clicks.count, 1)
            verify(!row.activeFocus, "a click keeps the keyboard where it was")
            row.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            keyClick(Qt.Key_Return)
            keyClick(Qt.Key_Enter)
            compare(clicks.count, 4)
        }

        function test_tab_shows_the_ring() {
            const ring = row.background.children[0]
            verify(!ring.visible)
            keyClick(Qt.Key_Tab)
            verify(row.activeFocus)
            verify(ring.visible)
            compare(ring.border.width, 2)
            compare(ring.radius, Theme.radius + 3)
            compare(ring.width, row.width + 6)
            keyClick(Qt.Key_Tab)
            verify(!ring.visible)
        }

        function test_accessible() {
            compare(row.Accessible.name, "Row")
        }

        function test_icon() {
            compare(icon.width, 18)
            compare(icon.height, 18)
            compare(icon.sourceSize, Qt.size(18, 18))
            compare(String(icon.source), Theme.icons + "go-previous")
            compare(String(none.source), "")
        }

        function test_chevron() {
            compare(Qt.size(right.width, right.height), Qt.size(6, 10))
            compare(Qt.size(down.width, down.height), Qt.size(9, 5))
        }
    }
}
