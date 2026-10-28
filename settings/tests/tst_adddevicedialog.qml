// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Bluetooth
import Vela.Controls
import "../qml"

// "Add a device" (docs/bluetooth.md §9.1): the search while it is open, a
// click to add, the others waiting, the question in place of the list,
// closing once added, Cancel and Esc stopping the Add, "Search again", and
// what it says about visibility.
Item {
    id: root
    width: 700
    height: 700

    AddDeviceDialog {
        id: dialog
    }

    TestCase {
        name: "AddDeviceDialog"
        when: windowShown

        function find(name) { return findChild(dialog, name) }
        function mouse() { return Bluetooth.devices[0] }
        function type(text) {
            for (const c of text)
                keyClick(c)
        }

        function init() {
            Bluetooth.reset()
            Bluetooth.devices = [Bluetooth.device(1, { name: "Mouse", rssi: -50 }),
                                 Bluetooth.device(2, { name: "Keyboard", icon: "input-keyboard" })]
            dialog.start()
            tryVerify(() => dialog.visible)
            waitForRendering(dialog)
        }
        function cleanup() {
            dialog.close()
            tryVerify(() => !dialog.visible)
        }

        function test_it_searches_and_lists_what_is_found() {
            compare(Bluetooth.calls, ["startSearch"])
            const list = find("found")
            compare(list.count, 2)
            verify(find("device:Mouse").enabled)
            mouseClick(find("device:Mouse"))
            compare(Bluetooth.calls, ["startSearch", "add /org/bluez/hci0/dev_1"])
            compare(dialog.adding, "/org/bluez/hci0/dev_1")
        }

        // While one pairs, the others wait (§6.5).
        function test_the_others_wait() {
            mouseClick(find("device:Mouse"))
            Bluetooth.pairing = mouse().path
            Bluetooth.devices = [Bluetooth.device(1, { name: "Mouse", operation: "pairing" }), Bluetooth.devices[1]]
            verify(find("wait").visible)
            compare(find("wait").text, "Wait for Mouse to finish pairing.")
            verify(!find("device:Keyboard").enabled)
            mouseClick(find("device:Keyboard"))
            compare(Bluetooth.calls.filter(c => c.startsWith("add")).length, 1)
        }

        function test_the_question_replaces_the_list() {
            mouseClick(find("device:Mouse"))
            Bluetooth.question = { id: "3", kind: "confirm-passkey", device: mouse().path, name: "Mouse", passkey: "123456",
                                   drawnBy: "settings", unsolicited: false }
            const question = find("question")
            verify(question.visible)
            verify(!find("found").visible)
            // One Cancel: the dialog's.
            verify(!findChild(question, "decline").visible)
            tryVerify(() => question.activeFocus)
            keyClick(Qt.Key_Return)
            compare(Bluetooth.calls[Bluetooth.calls.length - 1], "answer 3 accept")
            // The shell's questions aren't drawn here.
            Bluetooth.question = Object.assign({}, Bluetooth.question, { drawnBy: "shell" })
            verify(!question.visible)
            verify(find("found").visible)
        }

        function test_typing_the_code() {
            mouseClick(find("device:Mouse"))
            Bluetooth.question = { id: "4", kind: "enter-passkey", device: mouse().path, name: "Mouse", drawnBy: "settings" }
            const field = findChild(find("question"), "field")
            tryVerify(() => field.activeFocus)
            type("654321")
            keyClick(Qt.Key_Return)
            compare(Bluetooth.calls[Bluetooth.calls.length - 1], "answer 4 accept 654321")
        }

        function test_added_closes_it() {
            mouseClick(find("device:Mouse"))
            Bluetooth.added("/org/bluez/hci0/dev_2")
            verify(dialog.visible && !dialog.closing)
            Bluetooth.added(mouse().path)
            tryVerify(() => !dialog.visible)
            compare(Bluetooth.calls[Bluetooth.calls.length - 1], "stopSearch")
            verify(!Bluetooth.calls.some(c => c.startsWith("cancel")))
        }

        // Cancel and Esc stop the Add and the search.
        function test_cancel_stops_the_add() {
            mouseClick(find("device:Mouse"))
            Bluetooth.pairing = mouse().path
            mouseClick(findChild(dialog, "button:cancel"))
            tryVerify(() => !dialog.visible)
            compare(Bluetooth.calls.slice(-2), ["cancel /org/bluez/hci0/dev_1", "stopSearch"])
        }

        function test_escape_while_asked() {
            mouseClick(find("device:Mouse"))
            Bluetooth.pairing = mouse().path
            Bluetooth.question = { id: "5", kind: "enter-pin", device: mouse().path, name: "Mouse", drawnBy: "settings" }
            tryVerify(() => findChild(find("question"), "field").activeFocus)
            keyClick(Qt.Key_Escape)
            tryVerify(() => !dialog.visible)
            compare(Bluetooth.calls.slice(-2), ["cancel /org/bluez/hci0/dev_1", "stopSearch"])
        }

        function test_closing_without_an_add() {
            keyClick(Qt.Key_Escape)
            tryVerify(() => !dialog.visible)
            compare(Bluetooth.calls, ["startSearch", "stopSearch"])
        }

        // When the lease ends the list stays, with "Search again".
        function test_search_again() {
            Bluetooth.searching = true
            verify(!find("searchAgain").visible)
            Bluetooth.searching = false
            Bluetooth.searchEnded = true
            verify(find("searchAgain").visible)
            compare(find("found").count, 2)
            waitForRendering(dialog)
            mouseClick(find("searchAgain"))
            compare(Bluetooth.calls, ["startSearch", "startSearch"])
        }

        // What BlueZ says, never the lease (§6.6, rule 2).
        function test_visibility() {
            verify(!find("visibility").visible)
            Bluetooth.adapter = Object.assign({}, Bluetooth.adapter, { discoverable: true, discovering: true })
            compare(find("visibility").text, "Discoverable as \"vela-pc\"")
            Bluetooth.searching = true
            Bluetooth.adapter = Object.assign({}, Bluetooth.adapter, { discoverable: false })
            verify(find("visibility").text.startsWith("Another app is searching for devices"))
        }

        function test_a_refusal_is_shown() {
            Bluetooth.error = "Wait for Phone to finish pairing."
            verify(find("error").visible)
            compare(find("error").text, "Wait for Phone to finish pairing.")
        }
    }
}
