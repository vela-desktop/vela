// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls

// Slider: Qt's input (a binding on `value` survives it), `released(value)`
// once the user is done, the knob's center growing and shrinking with a fade,
// and the value itself never animated (docs/visual-coherence.md §6.8,
// invariant 6).
Item {
    id: root
    width: 400
    height: 300

    property real backing: 3
    property real committed: NaN

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    Slider {
        id: control
        x: 20
        y: 20
        width: 220
        from: 0
        to: 10
        stepSize: 1
        value: root.backing
        onMoved: root.backing = value
    }
    Slider {
        id: deferred
        x: 20
        y: 80
        width: 220
        from: 0
        to: 10
        stepSize: 1
        live: false
        mark: 5
        resetValue: 5
        value: root.committed
        onReleased: v => root.committed = v
    }
    SignalSpy { id: releases; target: deferred; signalName: "released" }

    TestCase {
        name: "Slider"
        when: windowShown

        readonly property Item dot: control.handle.children[0]

        function init() {
            mouseMove(root, 390, 290)
            // Qt Quick Test's synthetic click can leave this slider's
            // PointHandler hover cached between test functions offscreen.
            // Reset the hover subscription, not the production control's
            // geometry or bindings, before asserting the rest state.
            control.hoverEnabled = false
            control.hoverEnabled = true
            tryCompare(control, "hovered", false, 2000)
            start.forceActiveFocus()
            root.backing = 3
            root.committed = 2
            releases.clear()
            tryCompare(dot, "width", 12, 2000)
        }

        // Where `v` is on the track, in the slider's coordinates.
        function xOf(s, v) {
            return s.trackStart + s.trackLength * (v - s.from) / (s.to - s.from)
        }
        function handleCenter(s) {
            return s.handle.x + s.handle.width / 2
        }

        function test_a_click_moves_the_value_at_once() {
            mouseClick(control, xOf(control, 8), control.height / 2)
            compare(control.value, 8)
            compare(root.backing, 8)
            // Never animated: the knob is there in the same frame.
            compare(handleCenter(control), xOf(control, 8))
            const fill = control.background.children[2]
            compare(fill.width, control.trackLength * 0.8)
        }

        function test_the_binding_survives_input() {
            mouseClick(control, xOf(control, 6), control.height / 2)
            compare(root.backing, 6)
            root.backing = 1
            compare(control.value, 1)
            compare(handleCenter(control), xOf(control, 1))
        }

        function test_not_live_commits_on_release() {
            const y = deferred.height / 2
            mousePress(deferred, xOf(deferred, 2), y)
            mouseMove(deferred, xOf(deferred, 5), y)
            mouseMove(deferred, xOf(deferred, 7), y)
            compare(releases.count, 0)
            compare(root.committed, 2)
            mouseRelease(deferred, xOf(deferred, 7), y)
            compare(releases.count, 1)
            compare(releases.signalArguments[0][0], 7)
            compare(root.committed, 7)
            compare(deferred.value, 7)
        }

        function test_keys_and_wheel_commit() {
            deferred.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Right)
            compare(releases.count, 1)
            compare(root.committed, 3)
            keyClick(Qt.Key_Left)
            compare(root.committed, 2)
            mouseWheel(deferred, xOf(deferred, 2), deferred.height / 2, 0, 120)
            compare(root.committed, 3)
            compare(releases.count, 3)
        }

        function test_double_click_goes_back_to_the_reset_value() {
            const x = handleCenter(deferred)
            mouseDoubleClickSequence(deferred, x, deferred.height / 2)
            compare(releases.signalArguments[releases.count - 1][0], 5)
            compare(root.committed, 5)
            compare(deferred.value, 5)
        }

        function test_mark() {
            const tick = deferred.background.children[1]
            verify(tick.visible)
            compare(tick.x + tick.width / 2, xOf(deferred, 5))
            verify(!control.background.children[1].visible)
        }

        function test_the_knob_center_fades_with_hover_and_press() {
            const x = handleCenter(control)
            mouseMove(control, x, control.height / 2)
            if (Theme.animationEffects) {
                wait(Theme.fast / 3)
                verify(dot.width > 12 && dot.width < 14, "growing: " + dot.width)
            }
            tryCompare(dot, "width", 14, 2000)
            mousePress(control, x, control.height / 2)
            tryCompare(dot, "width", 10, 2000)
            mouseRelease(control, x, control.height / 2)
            tryCompare(dot, "width", 14, 2000)
        }

        function test_hover_is_instant_without_effects() {
            if (Theme.animationEffects)
                skip("with effects on")
            const x = handleCenter(control)
            mouseMove(control, x, control.height / 2)
            compare(control.hovered, true)
            compare(dot.width, 14)
            mousePress(control, x, control.height / 2)
            compare(dot.width, 10)
            mouseRelease(control, x, control.height / 2)
            compare(dot.width, 14)
            mouseMove(root, 390, 290)
            compare(dot.width, 12)
        }

        function test_tab_focuses_and_shows_the_ring() {
            const ring = control.handle.children[1]
            verify(!ring.visible)
            keyClick(Qt.Key_Tab)
            verify(control.activeFocus)
            verify(ring.visible)
            keyClick(Qt.Key_Right)
            compare(root.backing, 4)
        }

        function test_accessible() {
            compare(control.Accessible.role, Accessible.Slider)
        }
    }
}
