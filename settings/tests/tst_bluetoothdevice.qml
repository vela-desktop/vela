// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Bluetooth
import Vela.Controls
import "../qml"

// A paired device on the Bluetooth page (docs/bluetooth.md §9.1): its state
// and battery, Cancel while a step can be stopped, and its menu.
Item {
    id: root
    width: 700
    height: 400

    property var asked: []

    BluetoothDevice {
        id: card
        onRenameAsked: root.asked.push("rename")
        onRemoveAsked: root.asked.push("remove")
        onSoundAsked: root.asked.push("sound")
    }

    TestCase {
        name: "BluetoothDevice"
        when: windowShown

        function find(name) { return findChild(card, name) }

        function init() {
            Bluetooth.reset()
            root.asked = []
            card.hasSound = false
            card.device = Bluetooth.device(1, { name: "Headset", icon: "audio-headset", paired: true, connected: true })
        }

        // The menu's entries, as shown.
        function entries() {
            const more = find("more")
            mouseClick(more)
            const menu = findChild(more, "menu")
            tryVerify(() => menu.opened)
            const list = []
            for (const entry of menu.contentItem.children)
                if (entry.text !== undefined && entry.visible)
                    list.push(entry.text)
            return list
        }
        function choose(text) {
            const more = find("more")
            const menu = findChild(more, "menu")
            if (!menu.opened) {
                mouseClick(more)
                tryVerify(() => menu.opened)
            }
            mouseClick(findChild(menu.contentItem, "entry:" + text))
            tryVerify(() => !menu.visible)
        }

        function test_state_and_battery() {
            compare(card.title, "Headset")
            compare(card.description, "Connected")
            card.device = Object.assign({}, card.device, { battery: 80 })
            compare(card.description, "Connected · Battery 80%")
            card.device = Object.assign({}, card.device, { connected: false, battery: -1 })
            compare(card.description, "Paired")
            card.device = Object.assign({}, card.device, { operation: "connecting" })
            compare(card.description, "Connecting...")
            card.device = Object.assign({}, card.device, { operation: "", message: "The device isn't responding. Make sure it's on and nearby." })
            compare(card.description, "The device isn't responding. Make sure it's on and nearby.")
            card.device = Object.assign({}, card.device, { operation: "pairing", waiting: true, message: "" })
            compare(card.description, "Waiting for you...")
            card.device = Object.assign({}, card.device, { waiting: false })
            compare(card.description, "Pairing...")
        }

        function test_no_icon_from_bluez() {
            card.device = Object.assign({}, card.device, { icon: "bluetooth" })
            compare(card.icon, "preferences-system-bluetooth")
        }

        function test_cancel_while_connecting() {
            verify(!find("cancel").visible)
            card.device = Object.assign({}, card.device, { connected: false, operation: "connecting" })
            verify(find("cancel").visible)
            verify(!find("more").enabled)
            waitForRendering(card)
            mouseClick(find("cancel"))
            compare(Bluetooth.calls, ["cancel /org/bluez/hci0/dev_1"])
            // Disconnect, Remove and Rename run to their end.
            card.device = Object.assign({}, card.device, { operation: "disconnecting" })
            verify(!find("cancel").visible)
        }

        function test_the_menu() {
            compare(entries(), ["Disconnect", "Rename", "Remove"])
            choose("Disconnect")
            compare(Bluetooth.calls, ["disconnect /org/bluez/hci0/dev_1"])
            card.device = Object.assign({}, card.device, { connected: false })
            card.hasSound = true
            compare(entries(), ["Connect", "Rename", "Remove", "Sound settings"])
            choose("Connect")
            compare(Bluetooth.calls, ["disconnect /org/bluez/hci0/dev_1", "connect /org/bluez/hci0/dev_1"])
            choose("Rename")
            choose("Remove")
            choose("Sound settings")
            compare(root.asked, ["rename", "remove", "sound"])
        }

        function test_the_menu_by_keyboard() {
            const more = find("more")
            more.forceActiveFocus()
            keyClick(Qt.Key_Space)
            const menu = findChild(more, "menu")
            tryVerify(() => menu.opened)
            verify(findChild(menu.contentItem, "entry:Disconnect").activeFocus)
            keyClick(Qt.Key_Down)
            verify(findChild(menu.contentItem, "entry:Rename").activeFocus)
            keyClick(Qt.Key_Return)
            tryVerify(() => !menu.visible)
            compare(root.asked, ["rename"])
            compare(more.Accessible.name, "More options")
        }

        function test_nothing_while_the_session_is_away() {
            Bluetooth.active = false
            verify(!find("more").enabled)
        }
    }
}
