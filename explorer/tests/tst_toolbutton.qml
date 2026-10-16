// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"
import "../../common/tests/colors.js" as Colors

// File Explorer's ToolButton: Qt's tool button with Vela's subtle colors,
// faded; `checked` rests on the hover color; Tab focuses it, a click doesn't;
// the tooltip after a moment (docs/visual-coherence.md §6.8).
Item {
    id: root
    width: 400
    height: 300

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    ToolButton { id: copy; x: 20; y: 20; icon.name: "edit-copy"; tooltip: "Copy (Ctrl+C)" }
    ToolButton { id: sort; x: 80; y: 20; icon.name: "view-sort"; text: "Sort"; menu: true }
    ToolButton { id: pane; x: 200; y: 20; icon.name: "help-about"; text: "Details"; checked: true }
    ToolButton { id: paste; x: 20; y: 80; icon.name: "edit-paste"; usable: false }
    SignalSpy { id: clicks; target: copy; signalName: "clicked" }

    TestCase {
        name: "ToolButton"
        when: windowShown

        function init() {
            mouseMove(root, 390, 290)
            start.forceActiveFocus()
            clicks.clear()
            tryVerify(() => Colors.same(copy.background.color, Theme.subtleRest), 2000)
        }

        function test_states_reach_their_colors() {
            mouseMove(copy, 5, 5)
            tryVerify(() => Colors.same(copy.background.color, Theme.subtleHover), 2000, "hovered")
            mousePress(copy, 5, 5)
            tryVerify(() => Colors.same(copy.background.color, Theme.subtlePressed), 2000, "pressed")
            mouseRelease(copy, 5, 5)
            mouseMove(root, 390, 290)
            tryVerify(() => Colors.same(copy.background.color, Theme.subtleRest), 2000, "back at rest")
            verify(Colors.same(pane.background.color, Theme.subtleHover), "checked rests on the hover color")
        }

        function test_hover_fades() {
            mouseMove(copy, 5, 5)
            wait(Theme.fast / 3)
            verify(Colors.between(copy.background.color, Theme.subtleRest, Theme.subtleHover))
        }

        function test_size_and_content() {
            compare(copy.width, 32)
            compare(copy.height, 32)
            verify(sort.width > 60)
            const content = sort.contentItem.children[0]
            verify(content.children[2].visible, "the menu's arrow")
            verify(!copy.contentItem.children[0].children[2].visible)
            compare(String(content.children[0].source), Theme.icons + "view-sort")
        }

        function test_click_space_and_enter_activate_tab_shows_the_ring() {
            mouseClick(copy)
            compare(clicks.count, 1)
            verify(!copy.activeFocus, "a click keeps the keyboard with the view")
            keyClick(Qt.Key_Tab)
            verify(copy.activeFocus)
            verify(copy.background.children[0].visible)
            keyClick(Qt.Key_Space)
            keyClick(Qt.Key_Return)
            keyClick(Qt.Key_Enter)
            compare(clicks.count, 4)
            mouseClick(pane)
            verify(pane.checked, "the caller's binding decides, not a click")
        }

        function test_unusable() {
            compare(paste.enabled, false)
            compare(paste.opacity, 0.4)
            const spy = createTemporaryObject(spyComponent, root, { target: paste, signalName: "clicked" })
            mouseClick(paste)
            compare(spy.count, 0)
        }

        function test_tooltip() {
            compare(copy.Accessible.name, "Copy (Ctrl+C)")
            compare(sort.Accessible.name, "Sort")
            // Under the button or, once shown, in the window's content.
            const tip = findChild(copy, "tooltip") || findChild(copy.Window.window.contentItem, "tooltip")
            mouseMove(copy, 5, 5)
            verify(!tip.visible)
            tryVerify(() => tip.visible, 2000, "after a moment")
            mouseClick(copy)
            verify(!tip.visible, "a click hides it")
        }

        Component { id: spyComponent; SignalSpy {} }
    }
}
