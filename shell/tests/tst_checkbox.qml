// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"
import "../../common/tests/colors.js" as Colors

// The shell's CheckBox (Properties): Qt's toggling (a binding on `checked`
// survives it), the box's fading colors, the check mark, focus
// (docs/visual-coherence.md §6.8).
Item {
    id: root
    width: 400
    height: 300

    property bool readOnly: false

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    CheckBox {
        id: check
        x: 20
        y: 20
        text: "Read-only"
        checked: root.readOnly
        onToggled: root.readOnly = !root.readOnly
    }
    CheckBox { id: hidden; x: 20; y: 60; text: "Hidden"; checked: true; usable: false }
    SignalSpy { id: toggles; target: check; signalName: "toggled" }

    TestCase {
        name: "CheckBox"
        when: windowShown

        readonly property Item box: check.indicator
        readonly property Item mark: check.indicator.children[0]

        function init() {
            mouseMove(root, 390, 290)
            start.forceActiveFocus()
            root.readOnly = false
            toggles.clear()
            tryVerify(() => Colors.same(box.color, Theme.control), 2000)
        }

        function test_click_toggles_and_the_binding_survives() {
            verify(!mark.visible)
            mouseClick(check)
            compare(toggles.count, 1)
            verify(root.readOnly)
            verify(check.checked)
            verify(mark.visible)
            root.readOnly = false
            verify(!check.checked, "the binding survived the click")
            mouseClick(check, 4, 4)
            verify(check.checked, "a click on the box toggles too")
        }

        function test_states_reach_their_colors() {
            mouseMove(check, 4, 4)
            tryVerify(() => Colors.same(box.color, Theme.controlHover), 2000, "hovered")
            root.readOnly = true
            tryVerify(() => Colors.same(box.color, Theme.accentFillHover), 2000, "checked and hovered")
            mouseMove(root, 390, 290)
            tryVerify(() => Colors.same(box.color, Theme.accentFill), 2000, "checked")
            compare(box.border.width, 0)
        }

        function test_hover_fades() {
            mouseMove(check, 4, 4)
            wait(Theme.fast / 3)
            verify(Colors.between(box.color, Theme.control, Theme.controlHover))
        }

        function test_space_toggles_tab_shows_the_ring() {
            mouseClick(check)
            verify(!check.activeFocus, "a click keeps the keyboard where it was")
            root.readOnly = false
            keyClick(Qt.Key_Tab)
            verify(check.activeFocus)
            verify(box.children[1].visible)
            keyClick(Qt.Key_Space)
            verify(root.readOnly)
            keyClick(Qt.Key_Tab)
            verify(!hidden.activeFocus, "an unusable box is skipped")
        }

        function test_unusable() {
            compare(hidden.opacity, 0.45)
            mouseClick(hidden)
            verify(hidden.checked)
        }

        function test_accessible() {
            compare(check.Accessible.role, Accessible.CheckBox)
            compare(check.Accessible.name, "Read-only")
        }
    }
}
