// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Bluetooth.Ui
import Vela.Controls

// The agent's questions drawn inline (docs/bluetooth.md §7.2, §9.3): each
// kind's words, code and answers, Enter, Esc left to the dialog, the
// field's focus, the time left and the accessible names.
Item {
    id: root
    width: 520
    height: 400

    property var answers: []
    property int escapes: 0

    Keys.onEscapePressed: root.escapes++

    BluetoothQuestion {
        id: view
        onAnswered: (response, value) => root.answers.push(value !== "" ? response + " " + value : response)
    }

    TestCase {
        name: "BluetoothQuestion"
        when: windowShown

        function find(name) { return findChild(view, name) }
        function type(text) {
            for (const c of text)
                keyClick(c)
        }

        function ask(kind, changes) {
            view.question = Object.assign({ id: "7", kind: kind, name: "Mouse", icon: "input-mouse",
                                            deadline: Date.now() + 60000 }, changes || {})
            view.takeFocus()
            waitForRendering(view)
        }

        function init() {
            root.answers = []
            root.escapes = 0
            view.question = {}
        }

        function test_confirm_passkey() {
            ask("confirm-passkey", { passkey: "012345" })
            compare(find("prompt").text, "Does Mouse show this code?")
            compare(find("code").text, "012345")
            compare(find("code").Accessible.name, "Code 0 1 2 3 4 5")
            compare(find("accept").text, "Pair")
            compare(find("decline").text, "Cancel")
            verify(!find("field").visible)
            verify(view.activeFocus)
            keyClick(Qt.Key_Return)
            compare(root.answers, ["accept"])
            mouseClick(find("decline"))
            compare(root.answers, ["accept", "reject"])
        }

        function test_authorize_pairing() {
            ask("authorize-pairing")
            compare(find("prompt").text, "Mouse wants to pair with this PC.")
            verify(!find("code").visible)
            compare(find("accept").text, "Allow")
            compare(find("decline").text, "Deny")
            mouseClick(find("decline"))
            compare(root.answers, ["reject"])
        }

        function test_authorize_service() {
            ask("authorize-service", { service: "Audio sink", uuid: "0000110b-0000-1000-8000-00805f9b34fb" })
            compare(find("prompt").text, "Allow Mouse to use Audio sink?")
            mouseClick(find("accept"))
            compare(root.answers, ["accept"])
        }

        function test_enter_passkey_takes_six_digits() {
            ask("enter-passkey")
            compare(find("prompt").text, "Type the 6-digit code shown on Mouse.")
            const field = find("field")
            verify(field.visible)
            verify(field.activeFocus)
            compare(field.Accessible.name, "Code")
            // Letters don't go in, and fewer than six digits can't be sent.
            keyClick(Qt.Key_A)
            type("12345")
            compare(field.text, "12345")
            verify(!find("accept").enabled)
            keyClick(Qt.Key_Return)
            compare(root.answers, [])
            type("67")
            compare(field.text, "123456")
            verify(find("accept").enabled)
            keyClick(Qt.Key_Return)
            compare(root.answers, ["accept 123456"])
            mouseClick(find("decline"))
            compare(root.answers, ["accept 123456", "cancel"])
        }

        function test_enter_pin() {
            ask("enter-pin")
            compare(find("prompt").text, "Type the PIN for Mouse.")
            compare(find("field").Accessible.name, "PIN")
            verify(!find("accept").enabled)
            type("0000")
            keyClick(Qt.Key_Enter)
            compare(root.answers, ["accept 0000"])
        }

        function test_a_new_question_starts_with_an_empty_field() {
            ask("enter-pin")
            type("12")
            ask("enter-pin", { id: "8" })
            compare(find("field").text, "")
        }

        function test_show_passkey_only_cancels() {
            ask("show-passkey", { passkey: "004321", entered: 0 })
            compare(find("prompt").text, "Type this code on Mouse, then press Enter on it.")
            compare(find("code").text, "004321")
            verify(!find("accept").visible)
            verify(!find("entered").visible)
            keyClick(Qt.Key_Return)
            compare(root.answers, [])
            view.question = Object.assign({}, view.question, { entered: 3 })
            verify(find("entered").visible)
            compare(find("entered").text, "3 of 6 digits typed")
            mouseClick(find("decline"))
            compare(root.answers, ["cancel"])
        }

        function test_show_pin() {
            ask("show-pin", { pin: "482913" })
            compare(find("code").text, "482913")
            compare(find("code").Accessible.name, "Code 4 8 2 9 1 3")
            compare(find("decline").text, "Cancel")
        }

        // Esc isn't the question's: the dialog cancels the Add.
        function test_escape_goes_up() {
            ask("enter-passkey")
            keyClick(Qt.Key_Escape)
            compare(root.escapes, 1)
            ask("confirm-passkey", { passkey: "123456" })
            keyClick(Qt.Key_Escape)
            compare(root.escapes, 2)
            compare(root.answers, [])
        }

        function test_time_left_in_the_last_15_seconds() {
            ask("confirm-passkey", { passkey: "123456", deadline: Date.now() + 30000 })
            verify(!find("timeLeft").visible)
            ask("confirm-passkey", { id: "9", passkey: "123456", deadline: Date.now() + 10500 })
            tryVerify(() => find("timeLeft").visible)
            verify(/^Time left: 1[01] s$/.test(find("timeLeft").text), find("timeLeft").text)
        }
    }
}
