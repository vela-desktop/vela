// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"
import "../../common/tests/colors.js" as Colors

// Settings' RadioButton (night light's schedule): Qt's choice among siblings
// (a binding on `checked` survives it), the circle's fading colors, the dot
// that grows on hover, focus (docs/visual-coherence.md §6.8).
Item {
    id: root
    width: 400
    height: 300

    property string schedule: "sunset"

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    Column {
        x: 20
        y: 20
        RadioButton {
            id: sunset
            text: "Sunset to sunrise"
            checked: root.schedule === "sunset"
            onClicked: root.schedule = "sunset"
        }
        RadioButton {
            id: hours
            text: "Set hours"
            checked: root.schedule === "hours"
            onClicked: root.schedule = "hours"
        }
        RadioButton { id: never; text: "Never"; usable: false }
    }

    TestCase {
        name: "RadioButton"
        when: windowShown

        function circle(r) { return r.indicator }
        function dot(r) { return r.indicator.children[0] }

        function init() {
            mouseMove(root, 390, 290)
            start.forceActiveFocus()
            root.schedule = "sunset"
            tryVerify(() => Colors.same(circle(sunset).color, Theme.accentFill), 2000)
            tryVerify(() => Colors.same(circle(hours).color, Theme.control), 2000)
        }

        function test_a_click_chooses_and_the_bindings_survive() {
            mouseClick(hours)
            compare(root.schedule, "hours")
            verify(hours.checked)
            verify(!sunset.checked)
            root.schedule = "sunset"
            verify(sunset.checked, "the bindings survived the click")
            verify(!hours.checked)
        }

        function test_states_reach_their_colors() {
            mouseMove(hours, 4, 10)
            tryVerify(() => Colors.same(circle(hours).color, Theme.controlHover), 2000, "hovered")
            mouseMove(sunset, 4, 10)
            tryVerify(() => Colors.same(circle(sunset).color, Theme.accentFillHover), 2000, "chosen and hovered")
            tryVerify(() => Colors.same(circle(hours).color, Theme.control), 2000, "back at rest")
            verify(!dot(hours).visible)
        }

        function test_the_dot_grows_on_hover() {
            compare(dot(sunset).width, 8)
            mouseMove(sunset, 4, 10)
            tryCompare(dot(sunset), "width", 10, 2000)
            if (Theme.animationEffects) {
                mouseMove(root, 390, 290)
                wait(Theme.fast / 3)
                verify(dot(sunset).width > 8 && dot(sunset).width < 10, "it shrinks smoothly: " + dot(sunset).width)
            }
        }

        function test_tab_and_space() {
            keyClick(Qt.Key_Tab)
            verify(sunset.activeFocus)
            verify(circle(sunset).children[1].visible)
            keyClick(Qt.Key_Tab)
            verify(hours.activeFocus)
            keyClick(Qt.Key_Space)
            compare(root.schedule, "hours")
            keyClick(Qt.Key_Tab)
            verify(!never.activeFocus, "an unusable one is skipped")
        }

        function test_accessible() {
            compare(hours.Accessible.role, Accessible.RadioButton)
            compare(hours.Accessible.name, "Set hours")
            compare(never.opacity, 0.45)
        }
    }
}
