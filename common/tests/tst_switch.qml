// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "colors.js" as Colors

// Switch: Qt's toggling (a binding on `checked` survives it), the track's
// fading colors, a knob that follows a drag and slides on its own, or jumps
// with animation effects off (docs/visual-coherence.md §6.8).
Item {
    id: root
    width: 400
    height: 300

    property bool backing: false

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    Switch {
        id: control
        x: 20
        y: 20
        checked: root.backing
        onToggled: root.backing = checked
    }
    Switch { id: bare; x: 20; y: 80; showLabel: false }
    SignalSpy { id: toggles; target: control; signalName: "toggled" }

    TestCase {
        name: "Switch"
        when: windowShown

        readonly property Item track: control.indicator
        readonly property Item knob: control.indicator.children[0]

        function init() {
            mouseMove(root, 390, 290)
            start.forceActiveFocus()
            root.backing = false
            toggles.clear()
            tryVerify(() => Colors.same(track.color, Qt.rgba(0, 0, 0, 0.1)), 2000)
            tryCompare(knob, "x", 4, 2000)
        }

        function knobEnd() {
            return track.width - knob.width - 4
        }

        function test_label_and_size() {
            verify(control.contentItem.visible)
            compare(control.contentItem.text, "Off")
            verify(control.width > track.width + 12)
            compare(bare.width, 40)
            verify(!bare.contentItem.visible)
        }

        function test_click_toggles_and_the_binding_survives() {
            mouseClick(control)
            compare(toggles.count, 1)
            compare(control.checked, true)
            compare(root.backing, true)
            // The backend changes its mind: the switch follows.
            root.backing = false
            compare(control.checked, false)
            root.backing = true
            compare(control.checked, true)
            compare(control.contentItem.text, "On")
            tryVerify(() => Colors.same(track.color, Theme.accentFill) || Colors.same(track.color, Theme.accentFillHover), 2000)
        }

        function test_states_reach_their_colors() {
            mouseMove(track, 5, 5)
            tryVerify(() => Colors.same(track.color, Theme.controlHover), 2000, "off, hovered")
            mousePress(track, 5, 5)
            tryVerify(() => Colors.same(track.color, Theme.controlPressed), 2000, "off, pressed")
            mouseRelease(track, 5, 5)
            compare(control.checked, true)
            tryVerify(() => Colors.same(track.color, Theme.accentFillHover), 2000, "on, hovered")
            mouseMove(root, 390, 290)
            tryVerify(() => Colors.same(track.color, Theme.accentFill), 2000, "on")
            tryVerify(() => Colors.same(knob.color, Theme.accentText), 2000, "the knob on the accent")
        }

        function test_knob_slides_when_toggled() {
            if (!Theme.animationEffects)
                skip("with effects on")
            control.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            compare(control.checked, true)
            wait(Theme.fast / 3)
            verify(knob.x > 4 && knob.x < knobEnd(), "on its way: " + knob.x)
            tryCompare(knob, "x", knobEnd(), 2000)
        }

        function test_knob_jumps_without_effects() {
            if (Theme.animationEffects)
                skip("with effects off")
            control.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            compare(control.checked, true)
            compare(knob.x, knobEnd())
        }

        function test_knob_size_is_instant_without_effects() {
            if (Theme.animationEffects)
                skip("with effects on")
            mouseMove(track, 5, 5)
            compare(control.hovered, true)
            compare(knob.width, 14)
            compare(knob.height, 14)
            mousePress(track, 5, 5)
            compare(knob.width, 17)
            mouseRelease(track, 5, 5)
            compare(knob.width, 14)
            mouseMove(root, 390, 290)
            compare(knob.width, 12)
            compare(knob.height, 12)
        }

        function test_knob_follows_a_drag() {
            const y = track.height / 2
            mousePress(track, 6, y)
            // Qt starts the drag past its threshold; then the knob is under
            // the pointer at once, without sliding.
            mouseMove(track, 20, y)
            mouseMove(track, 26, y)
            verify(control.visualPosition > 0 && control.visualPosition < 1, "position " + control.visualPosition)
            compare(knob.x, 4 + control.visualPosition * (track.width - knob.width - 8))
            mouseMove(track, 40, y)
            mouseRelease(track, 40, y)
            compare(control.checked, true)
            compare(root.backing, true)
        }

        function test_space_toggles_tab_focuses_and_shows_the_ring() {
            const ring = track.children[1]
            mouseClick(control)
            verify(!control.activeFocus, "a click keeps the keyboard where it was")
            keyClick(Qt.Key_Tab)
            verify(control.activeFocus)
            verify(ring.visible)
            keyClick(Qt.Key_Space)
            compare(control.checked, false)
            compare(toggles.count, 2)
        }

        function test_accessible() {
            verify(control.Accessible.checkable)
            compare(control.Accessible.checked, false)
            mouseClick(control)
            compare(control.Accessible.checked, true)
        }

        function test_unusable() {
            bare.usable = false
            compare(bare.enabled, false)
            compare(bare.opacity, 0.45)
            mouseClick(bare)
            compare(bare.checked, false)
            bare.usable = true
        }
    }
}
