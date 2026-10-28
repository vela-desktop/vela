// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Bluetooth
import Vela.Controls
import "../qml"

// The shell's dialog for the Bluetooth agent's questions
// (docs/bluetooth.md §9.3): it opens on the primary screen when
// BluetoothPrompts has a question, shows it, sends Enter, Esc and the
// buttons' answers, shows a refused answer's reason, follows the digits
// typed on a keyboard, stays when another window is active, and closes when
// the question does.
Item {
    id: root
    width: 200
    height: 200

    // A window of its own, as in the shell: not this one's transient child,
    // which would be active along with it.
    BluetoothDialog {
        id: dialog
        transientParent: null
    }

    TestCase {
        name: "BluetoothDialog"
        when: windowShown

        function find(name) { return findChild(dialog.contentItem, name) }
        function type(text) {
            for (const c of text)
                keyClick(c)
        }
        function ask(kind, changes) {
            BluetoothPrompts.dialog = Object.assign({ id: "7", kind: kind, name: "Pixel 9", icon: "phone",
                                                      deadline: Date.now() + 60000, drawnBy: "shell" }, changes || {})
            BluetoothPrompts.dialogChanged()
        }
        function close() {
            BluetoothPrompts.dialog = {}
            BluetoothPrompts.dialogChanged()
        }
        function shownActive() {
            tryVerify(() => dialog.visible)
            // Offscreen fixtures need explicit activation to accept keys.
            // This does not prove compositor-driven production activation.
            dialog.requestActivate()
            tryVerify(() => dialog.active)
            waitForRendering(dialog.contentItem)
        }

        function init() {
            Bluetooth.reset()
            BluetoothPrompts.reset()
            Shell.placed = []
        }
        function cleanup() {
            close()
            tryVerify(() => !dialog.visible)
        }

        function test_opens_on_the_primary_screen_and_closes_with_the_question() {
            verify(!dialog.visible)
            ask("confirm-passkey", { passkey: "123456" })
            shownActive()
            compare(Shell.placed, ["test-output"])
            compare(find("prompt").text, "Does Pixel 9 show this code?")
            compare(find("code").text, "123456")
            compare(find("bluetoothDialog").Accessible.role, Accessible.Dialog)
            close()
            tryVerify(() => !dialog.visible)
            compare(BluetoothPrompts.answers, [])
        }

        function test_device_name_with_markup_is_literal() {
            ask("confirm-passkey", { name: "<b>Phone</b>", passkey: "123456" })
            shownActive()
            compare(find("prompt").text, "Does <b>Phone</b> show this code?")
            compare(find("prompt").textFormat, Text.PlainText)
        }

        function test_enter_and_esc() {
            ask("confirm-passkey", { passkey: "123456" })
            shownActive()
            keyClick(Qt.Key_Return)
            compare(BluetoothPrompts.answers, ["accept"])
            // It waits for the question to close, wherever it was answered.
            verify(dialog.visible)
            keyClick(Qt.Key_Escape)
            compare(BluetoothPrompts.answers, ["accept", "reject"])
            ask("enter-pin", { id: "8" })
            keyClick(Qt.Key_Escape)
            compare(BluetoothPrompts.answers, ["accept", "reject", "cancel"])
        }

        // The field has focus: the code typed goes with Enter.
        function test_a_code_typed_here() {
            ask("enter-passkey")
            shownActive()
            verify(find("field").activeFocus)
            type("654321")
            keyClick(Qt.Key_Return)
            compare(BluetoothPrompts.answers, ["accept 654321"])
        }

        function test_buttons() {
            ask("authorize-service", { service: "audio streaming" })
            shownActive()
            compare(find("prompt").text, "Allow Pixel 9 to use audio streaming?")
            mouseClick(find("decline"))
            compare(BluetoothPrompts.answers, ["reject"])
            mouseClick(find("accept"))
            compare(BluetoothPrompts.answers, ["reject", "accept"])
        }

        // The digits typed on a keyboard, as the same question changes.
        function test_follows_the_question() {
            ask("show-passkey", { passkey: "000123", entered: 0 })
            shownActive()
            verify(!find("entered").visible)
            verify(!find("accept").visible)
            ask("show-passkey", { passkey: "000123", entered: 3 })
            compare(find("entered").text, "3 of 6 digits typed")
            compare(Shell.placed.length, 1)
        }

        function test_a_refused_answer_says_why() {
            Bluetooth.error = "stale"
            ask("enter-pin")
            shownActive()
            // A new question starts without the last one's refusal.
            compare(Bluetooth.error, "")
            Bluetooth.error = "That code isn't valid."
            verify(findChild(dialog.contentItem, "error").visible)
            compare(findChild(dialog.contentItem, "error").text, "That code isn't valid.")
        }

        // Another window active: the device is still waiting.
        function test_stays_when_another_window_is_active() {
            ask("confirm-passkey", { passkey: "123456" })
            shownActive()
            root.Window.window.requestActivate()
            tryVerify(() => !dialog.active)
            wait(100)
            verify(dialog.visible)
            compare(BluetoothPrompts.answers, [])
        }
    }
}
