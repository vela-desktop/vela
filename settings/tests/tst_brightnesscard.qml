// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Settings' brightness card (docs/brightness.md §8) for each state of the
// compositor's model: a slider, a locked one, a refusal's line, the reasons
// without a slider, and nothing where there is nothing to say.
Item {
    id: root
    width: 700
    height: 200

    property var moves: []

    function entry(reason, changes) {
        const available = reason === "ready" || reason === "not-responding"
        return Object.assign({ name: "DP-3", label: "Mi Monitor", mechanism: "ddc", available: available,
                               locked: reason === "not-responding", pending: false, refused: false,
                               reason: reason, value: available ? 0.7 : 0 }, changes || {})
    }

    BrightnessCard {
        id: card
        onMoved: value => root.moves.push(value)
    }

    TestCase {
        name: "BrightnessCard"
        when: windowShown

        function slider() { return card.trailing[0] }

        function init() {
            root.moves = []
            card.brightness = undefined
        }

        function test_a_slider_that_moves_its_display() {
            card.brightness = root.entry("ready")
            verify(card.visible)
            verify(slider().visible)
            verify(slider().usable)
            compare(slider().value, 0.7)
            compare(card.description, "Adjust the brightness of this display")
            compare(slider().Accessible.name, "Brightness")
            waitForRendering(card)
            mouseClick(slider(), slider().trackStart + slider().trackLength * 0.25, slider().height / 2)
            compare(root.moves.length, 1)
            fuzzyCompare(root.moves[0], 0.25, 0.02)
        }

        function test_locked_while_the_display_is_not_responding() {
            card.brightness = root.entry("not-responding")
            verify(card.visible)
            verify(slider().visible)
            verify(!slider().usable)
            compare(slider().value, 0.7)
            compare(card.description, "This display isn't responding")
        }

        function test_a_refusal_says_so() {
            card.brightness = root.entry("ready", { refused: true, value: 0.4 })
            verify(slider().usable)
            compare(slider().value, 0.4)
            compare(card.description, "The display didn't accept the change")
        }

        function test_the_reasons_without_a_slider_data() {
            return [
                { tag: "no-answer", text: "Brightness is controlled by the display's buttons. If its menu has a DDC/CI option, turning it on lets Vela control it." },
                { tag: "unsupported", text: "This display doesn't allow changing its brightness" },
                { tag: "no-access", text: "Vela can't reach this display. The installation may be incomplete" }
            ]
        }
        function test_the_reasons_without_a_slider(data) {
            card.brightness = root.entry(data.tag)
            verify(card.visible)
            verify(!slider().visible)
            compare(card.description, data.text)
        }

        function test_nothing_to_say_data() {
            return [
                { tag: "no-bus", brightness: root.entry("no-bus") },
                { tag: "probing", brightness: root.entry("probing") },
                { tag: "off", brightness: root.entry("off") },
                // Not responding before it ever answered: no value to show.
                { tag: "never answered", brightness: root.entry("not-responding", { available: false }) },
                { tag: "no helper", brightness: undefined }
            ]
        }
        function test_nothing_to_say(data) {
            card.brightness = data.brightness
            verify(!card.visible)
        }

        // A state that turns ready shows the slider in the same card.
        function test_from_a_reason_to_a_slider() {
            card.brightness = root.entry("no-answer")
            verify(!slider().visible)
            card.brightness = root.entry("ready")
            verify(slider().visible)
            compare(card.description, "Adjust the brightness of this display")
        }
    }
}
