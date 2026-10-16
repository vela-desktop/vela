// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "colors.js" as Colors

// Button: one color per state combination, faded; Qt's keyboard, focus and
// accessibility (docs/visual-coherence.md §6.8).
Item {
    id: root
    width: 400
    height: 300

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    Button { id: standard; x: 20; y: 20; text: "Standard" }
    Button { id: accent; x: 20; y: 70; text: "Accent"; accent: true }
    Button { id: subtle; x: 20; y: 120; text: "Subtle"; subtle: true }
    Button { id: unusable; x: 20; y: 170; text: "Unusable"; usable: false }
    SignalSpy { id: clicks; target: standard; signalName: "clicked" }

    TestCase {
        name: "Button"
        when: windowShown

        function init() {
            mouseMove(root, 390, 290)
            start.forceActiveFocus()
            clicks.clear()
            for (const b of [standard, accent, subtle])
                tryVerify(() => Colors.same(b.background.color, rest(b)), 2000)
        }

        function rest(b) {
            return b.subtle ? Theme.subtleRest : b.accent ? Theme.accentFill : Theme.control
        }
        function hover(b) {
            return b.subtle ? Theme.subtleHover : b.accent ? Theme.accentFillHover : Theme.controlHover
        }

        function test_rest_colors() {
            verify(Colors.same(standard.background.color, Theme.control))
            verify(Colors.same(accent.background.color, Theme.accentFill))
            verify(Colors.same(subtle.background.color, Theme.subtleRest))
            compare(unusable.enabled, false)
            compare(unusable.opacity, 0.45)
            compare(standard.opacity, 1)
        }

        function test_subtle_rest_is_its_hover_at_alpha_zero() {
            const h = Theme.subtleHover
            const r = Theme.subtleRest
            verify(Colors.same(Qt.rgba(r.r, r.g, r.b, h.a), h))
            compare(r.a, 0)
        }

        function test_states_reach_their_colors() {
            for (const b of [standard, accent, subtle]) {
                mouseMove(b, 10, 10)
                tryVerify(() => Colors.same(b.background.color, hover(b)), 2000, b.text + " hovered")
                mousePress(b, 10, 10)
                if (!b.accent)
                    tryVerify(() => Colors.same(b.background.color,
                                                b.subtle ? Theme.subtlePressed : Theme.controlPressed), 2000, b.text + " pressed")
                mouseRelease(b, 10, 10)
                mouseMove(root, 390, 290)
                tryVerify(() => Colors.same(b.background.color, rest(b)), 2000, b.text + " back at rest")
            }
        }

        function test_hover_fades_and_turns_back() {
            if (!Theme.animationEffects)
                skip("the fades take `fast` without effects too, but this checks the 3× duration")
            mouseMove(standard, 10, 10)
            wait(Theme.fast / 3)
            const halfway = Colors.copy(standard.background.color)
            verify(Colors.between(halfway, Theme.control, Theme.controlHover), "halfway: " + halfway)
            mouseMove(root, 390, 290)
            // From where it was, toward rest: never on to the full hover.
            let farthest = 0
            tryVerify(() => {
                const c = standard.background.color
                farthest = Math.max(farthest, Colors.distance(c, Theme.control))
                return Colors.distance(c, Theme.control) < Colors.distance(halfway, Theme.control) - 0.001
            }, 2000, "turning back from " + halfway)
            verify(farthest <= Colors.distance(halfway, Theme.control) + 0.002, "went on toward the hover")
            tryVerify(() => Colors.same(standard.background.color, Theme.control), 2000)
        }

        function test_fades_without_effects_too() {
            if (Theme.animationEffects)
                skip("with effects off")
            compare(Theme.fast, Theme.scale.fast)
            mouseMove(standard, 10, 10)
            wait(Theme.fast / 3)
            verify(Colors.between(standard.background.color, Theme.control, Theme.controlHover))
        }

        function test_space_and_enter_activate() {
            standard.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            compare(clicks.count, 1)
            keyClick(Qt.Key_Return)
            compare(clicks.count, 2)
            keyClick(Qt.Key_Enter)
            compare(clicks.count, 3)
        }

        function test_tab_focuses_and_shows_the_ring_a_click_does_not() {
            mouseClick(standard)
            compare(clicks.count, 1)
            verify(!standard.activeFocus, "a click keeps the keyboard where it was")
            verify(start.activeFocus)
            keyClick(Qt.Key_Tab)
            verify(standard.activeFocus)
            verify(standard.visualFocus)
            const ring = standard.background.children[0]
            verify(ring.visible)
            keyClick(Qt.Key_Tab)
            verify(accent.activeFocus)
            verify(!ring.visible)
            // An unusable button is skipped.
            keyClick(Qt.Key_Tab)
            verify(subtle.activeFocus)
            keyClick(Qt.Key_Tab)
            verify(!unusable.activeFocus)
        }

        function test_unusable_ignores_input() {
            const spy = createTemporaryObject(signalSpy, root, { target: unusable, signalName: "clicked" })
            mouseClick(unusable)
            compare(spy.count, 0)
        }

        function test_accessible() {
            compare(standard.Accessible.role, Accessible.Button)
            compare(standard.Accessible.name, "Standard")
        }

        Component { id: signalSpy; SignalSpy {} }
    }
}
