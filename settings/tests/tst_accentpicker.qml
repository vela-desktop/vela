// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Colors' custom accent (AccentPicker.qml, from AccentCard's "View
// colors"): it starts from the accent in use; the spectrum, the arrows, the
// brightness and a typed hex code change the color; Done chooses exactly it,
// Cancel nothing; a code that isn't one can't be chosen.
Item {
    id: root
    width: 900
    height: 800

    function navigate(name) {}

    Column {
        width: parent.width
        AccentCard {
            id: card
        }
    }

    TestCase {
        name: "AccentPicker"
        when: windowShown

        function picker() {
            return findChild(root, "accentPicker")
        }
        function child(name) {
            return findChild(picker(), name)
        }
        function hex() {
            return child("accentHex")
        }
        // The dialog's buttons are made again when Done's state changes:
        // their row places them at its next polish.
        function button(value) {
            const found = child("button:" + value)
            waitForItemPolished(found.parent)
            return found
        }
        function done() {
            return button("ok")
        }
        function open() {
            mouseClick(findChild(card, "accentViewColors"))
            // Done counts once its entrance is over (Dialog's PanelMotion).
            tryVerify(() => {
                for (let i = 0; i < picker().children.length; ++i) {
                    if (picker().children[i].opened === true)
                        return true
                }
                return false
            }, 3000)
        }
        function type(text) {
            hex().forceActiveFocus()
            hex().selectAll()
            keySequence(StandardKey.Delete)
            for (const c of text) {
                keyClick(c)
            }
        }

        function init() {
            Prefs.reset()
        }
        function cleanup() {
            picker().close()
            tryVerify(() => !picker().visible)
        }

        function test_starts_from_the_accent() {
            open()
            compare(hex().text, "#0078D4")
            compare(child("accentBefore").color, Prefs.accent)
            compare(child("accentAfter").color, Prefs.accent)
            verify(!findChild(card, "accentCustom").visible, "a palette color shown as custom")
        }

        function test_a_typed_code_is_chosen_exactly() {
            open()
            type("1e90ff") // without the #
            compare(child("accentAfter").color, Qt.color("#1e90ff"))
            verify(done().enabled)
            mouseClick(done())
            compare(Prefs.accent, Qt.color("#1e90ff"))
            // Not in the palette: the card shows it, with its code.
            verify(findChild(card, "accentCustom").visible)
            compare(findChild(card, "accentCustom").children[0].color, Qt.color("#1e90ff"))
        }

        function test_a_wrong_code_cant_be_chosen() {
            open()
            type("#12G45")
            verify(!done().enabled)
            verify(child("accentHexError").visible)
            keyClick(Qt.Key_Return)
            verify(picker().visible, "Enter chose a wrong code")
            compare(Prefs.accent, Qt.color("#0078d4"))
        }

        function test_cancel_changes_nothing() {
            open()
            type("#ff0000")
            mouseClick(button("cancel"))
            compare(Prefs.accent, Qt.color("#0078d4"))
        }

        function test_the_spectrum_and_the_arrows() {
            open()
            const spectrum = child("accentSpectrum")
            // Top left: red, at the accent's brightness.
            mouseClick(spectrum, 1, 1)
            const red = child("accentAfter").color
            verify(red.hsvHue < 0.01 || red.hsvHue > 0.99, red)
            verify(red.hsvSaturation > 0.98, red)
            compare(hex().text, red.toString().toUpperCase())
            // Right: towards yellow; down: less saturated.
            keyClick(Qt.Key_Right)
            verify(child("accentAfter").color.hsvHue > 0)
            const saturation = child("accentAfter").color.hsvSaturation
            keyClick(Qt.Key_Down)
            verify(child("accentAfter").color.hsvSaturation < saturation)
            mouseClick(done())
            compare(Prefs.accent, child("accentAfter").color)
        }

        function test_brightness() {
            open()
            const slider = child("accentBrightness")
            slider.forceActiveFocus()
            const value = child("accentAfter").color.hsvValue
            keyClick(Qt.Key_Left)
            keyClick(Qt.Key_Left)
            verify(child("accentAfter").color.hsvValue < value)
            // Black keeps the hue: brighter again, the same color comes back.
            slider.value = 0
            slider.moved()
            compare(hex().text, "#000000")
            slider.value = 100
            slider.moved()
            verify(Math.abs(child("accentAfter").color.hsvHue - Qt.color("#0078d4").hsvHue) < 0.01)
        }

        function test_disabled_with_the_themes_accent() {
            Prefs.tokens = { vela: { accent: "#ff00ff" } }
            ++Prefs.themesRevision
            verify(!findChild(card, "accentViewColors").enabled)
        }
    }
}
