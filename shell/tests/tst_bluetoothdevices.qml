// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Bluetooth
import Vela.Controls
import "../qml"

// Quick settings' paired devices (docs/bluetooth.md §9.2): connected first,
// state and battery, a click opening one with Connect, Disconnect or
// Cancel, nothing to press while the session is in the background, and the
// height the panel goes to.
Item {
    id: root
    width: 360
    height: 500

    BluetoothDevices {
        id: list
        width: 328
    }

    TestCase {
        name: "BluetoothDevices"
        when: windowShown

        function row(name) { return findChild(list, "device " + name) }
        function action(name) { return findChild(row(name), "action") }
        // Opens a device, and waits until it has finished opening.
        function open(name) {
            mouseClick(row(name), 20, 20)
            tryCompare(row(name), "height", 96)
            // The column moves the rows below at its next layout.
            tryVerify(laidOut)
        }
        function laidOut() {
            let y = 0
            for (const item of list.contentItem.children[0].children) {
                if (!item.objectName.startsWith("device "))
                    continue
                if (Math.abs(item.y - y) > 0.5)
                    return false
                y += item.height + 2
            }
            return true
        }
        function shown() {
            const names = []
            for (const item of list.contentItem.children[0].children)
                if (item.objectName.startsWith("device "))
                    names.push(item.objectName.substring(7))
            return names
        }

        function init() {
            Bluetooth.reset()
            list.expanded = ""
            // As the model gives them: connected first.
            Bluetooth.devices = [
                Bluetooth.device(1, { name: "Headset", icon: "audio-headset", paired: true, connected: true, battery: 80 }),
                Bluetooth.device(2, { name: "Mouse", paired: true }),
                Bluetooth.device(3, { name: "Phone", icon: "phone" })
            ]
            waitForRendering(list)
        }

        function test_paired_only_with_state_and_battery() {
            compare(shown(), ["Headset", "Mouse"])
            compare(findChild(row("Headset"), "state").text, "Connected · Battery 80%")
            compare(findChild(row("Mouse"), "state").text, "Paired")
            compare(row("Headset").Accessible.description, "Connected · Battery 80%")
        }

        function test_names_are_plain_text() {
            Bluetooth.devices = [Bluetooth.device(2, { name: "<b>Phone</b>", paired: true })]
            tryVerify(() => row("<b>Phone</b>") !== null)
            const label = findChild(row("<b>Phone</b>"), "deviceName")
            verify(label !== null)
            compare(label.text, "<b>Phone</b>")
            compare(label.textFormat, Text.PlainText)
        }

        function test_a_click_opens_one() {
            compare(list.finalHeight, 52 * 2 + 2)
            mouseClick(row("Mouse"), 20, 20)
            compare(list.expanded, Bluetooth.devices[1].path)
            // The panel goes to the final height before the row has opened
            // (the column lays out once per frame).
            tryCompare(list, "finalHeight", 52 * 2 + 2 + 44)
            verify(row("Mouse").height < 96 || Theme.motion === 0)
            tryCompare(row("Mouse"), "height", 96)
            verify(action("Mouse").visible)
            compare(action("Mouse").text, "Connect")
            mouseClick(action("Mouse"))
            compare(Bluetooth.calls, ["connect " + Bluetooth.devices[1].path])
            // The other one opens instead; a click there closes it.
            open("Headset")
            compare(list.expanded, Bluetooth.devices[0].path)
            compare(action("Headset").text, "Disconnect")
            mouseClick(action("Headset"))
            compare(Bluetooth.calls[1], "disconnect " + Bluetooth.devices[0].path)
            mouseClick(row("Headset"), 20, 20)
            compare(list.expanded, "")
        }

        function test_cancel_while_connecting() {
            Bluetooth.devices = [Bluetooth.device(2, { name: "Mouse", paired: true, operation: "connecting" })]
            open("Mouse")
            compare(findChild(row("Mouse"), "state").text, "Connecting...")
            compare(action("Mouse").text, "Cancel")
            mouseClick(action("Mouse"))
            compare(Bluetooth.calls, ["cancel " + Bluetooth.devices[0].path])
        }

        // Disconnecting can't be stopped: nothing to press meanwhile.
        function test_busy_and_background() {
            Bluetooth.devices = [Bluetooth.device(1, { name: "Headset", paired: true, connected: true, operation: "disconnecting" })]
            open("Headset")
            verify(!action("Headset").enabled)
            mouseClick(action("Headset"))
            compare(Bluetooth.calls, [])
            Bluetooth.devices = [Bluetooth.device(1, { name: "Headset", paired: true, connected: true })]
            Bluetooth.active = false
            verify(!action("Headset").enabled)
            Bluetooth.active = true
            verify(action("Headset").enabled)
        }

        function test_a_long_list_scrolls() {
            const many = []
            for (let i = 1; i <= 9; ++i)
                many.push(Bluetooth.device(i, { name: "Device " + i, paired: true }))
            Bluetooth.devices = many
            tryCompare(list, "height", 340)
            compare(list.finalHeight, 340)
            verify(list.contentHeight > 340)
        }
    }
}
