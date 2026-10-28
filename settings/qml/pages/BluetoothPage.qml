// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Bluetooth
import Vela.Controls

// Bluetooth & devices (docs/bluetooth.md §9.1), on vela-bluetooth: the
// switch, "Add a device", the paired devices (connected first) with their
// menu, and the adapter when there are two or more.
Page {
    id: page

    // A headset's sound card (vela-audio), by its address: WirePlumber
    // names it bluez_card.<address>.
    function soundCard(device) {
        const address = (device.address || "").replace(/:/g, "_")
        const card = Audio.devices.find(d => d.bluetooth && address !== "" && d.name.indexOf(address) >= 0)
        return card !== undefined ? card.name : ""
    }

    Card {
        objectName: "unavailable"
        visible: !Bluetooth.available && !Bluetooth.platformBlocked
        icon: "preferences-system-bluetooth-inactive"
        title: qsTr("Bluetooth isn't available on this PC")
        description: qsTr("There's no Bluetooth adapter, or the bluetooth service isn't running.")
    }

    Card {
        objectName: "power"
        visible: Bluetooth.available || Bluetooth.platformBlocked
        icon: Bluetooth.powered ? "preferences-system-bluetooth" : "preferences-system-bluetooth-inactive"
        title: qsTr("Bluetooth")
        // BlueZ's own Discoverable (§6.6, rule 2).
        description: !Bluetooth.powered ? qsTr("Off")
            : Bluetooth.adapter.discoverable ? qsTr("Discoverable as \"%1\"").arg(Bluetooth.adapter.name)
            : qsTr("On")
        minimumHeight: 80
        trailing: Switch {
            objectName: "switch"
            checked: Bluetooth.powered
            usable: Bluetooth.active
            Accessible.name: qsTr("Bluetooth")
            onToggled: Bluetooth.setPowered(checked)
        }
    }

    Card {
        objectName: "add"
        visible: Bluetooth.available
        icon: "list-add"
        title: qsTr("Devices")
        description: qsTr("Mouse, keyboard, pen, audio, displays and docks, other devices")
        trailing: Button {
            objectName: "addButton"
            text: qsTr("Add device")
            usable: Bluetooth.powered && Bluetooth.active
            onClicked: addDialog.start()
        }
    }

    Text {
        objectName: "error"
        visible: text !== "" && !addDialog.visible
        width: parent.width
        text: Bluetooth.error
        color: Theme.critical
        font.pixelSize: Theme.fontBody
        wrapMode: Text.Wrap
        topPadding: 8
        bottomPadding: 8
    }

    Repeater {
        model: Bluetooth.available ? Bluetooth.paired : []
        delegate: BluetoothDevice {
            required property var modelData
            device: modelData
            hasSound: page.soundCard(modelData) !== ""
            onRenameAsked: renameDialog.start(modelData)
            onRemoveAsked: {
                removeDialog.device = modelData
                removeDialog.open()
            }
            onSoundAsked: root.showSoundDevice("", "", page.soundCard(modelData))
        }
    }

    CardGroup {
        objectName: "advanced"
        title: qsTr("Advanced")
        visible: Bluetooth.adapters.length >= 2
        Card {
            icon: "preferences-system-bluetooth"
            title: qsTr("Bluetooth adapter")
            description: qsTr("The one this PC uses")
            trailing: Choice {
                model: Bluetooth.adapters.map(a => ({ text: a.name !== "" ? a.name + " (" + a.address + ")" : a.address }))
                currentIndex: Bluetooth.adapters.findIndex(a => a.inUse)
                usable: Bluetooth.active
                onChosen: index => Bluetooth.useAdapter(Bluetooth.adapters[index].address)
            }
        }
    }

    CardGroup {
        title: qsTr("Related settings")
        LinkCard {
            icon: "input-mouse"
            title: qsTr("Mouse")
            description: qsTr("Buttons, pointer speed, scrolling")
            onClicked: root.navigate("mouse")
        }
        LinkCard {
            visible: Prefs.hasTouchpad
            icon: "input-touchpad"
            title: qsTr("Touchpad")
            description: qsTr("Taps, gestures, scrolling")
            onClicked: root.navigate("touchpad")
        }
        LinkCard {
            icon: "audio-speakers"
            title: qsTr("Sound")
            description: qsTr("Where sound from connected devices goes")
            onClicked: root.navigate("sound")
        }
    }

    Dialog {
        id: removeDialog
        objectName: "removeDialog"
        parent: root.contentItem
        property var device: null
        title: qsTr("Remove this device?")
        buttons: [{ text: qsTr("Remove"), accent: true, value: "remove" }, { text: qsTr("Cancel"), value: "cancel" }]
        onChosen: value => { if (value === "remove") Bluetooth.removeDevice(device.path) }
        Text {
            width: parent.width
            text: removeDialog.device ? qsTr("To use %1 again, you'll need to pair it once more.").arg(removeDialog.device.name) : ""
            color: Theme.text
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
    }

    Dialog {
        id: renameDialog
        objectName: "renameDialog"
        parent: root.contentItem
        property var device: null
        title: qsTr("Rename this device")
        buttons: [{ text: qsTr("Rename"), accent: true, value: "rename", enabled: nameField.text.trim() !== "" },
                  { text: qsTr("Cancel"), value: "cancel" }]
        function start(d) {
            device = d
            nameField.text = d.name
            open()
            nameField.selectAll()
            nameField.forceActiveFocus()
        }
        onChosen: value => { if (value === "rename") Bluetooth.rename(device.path, nameField.text.trim()) }
        TextBox {
            id: nameField
            objectName: "nameField"
            width: parent.width
            maximumLength: 248
            Accessible.name: qsTr("Name")
            Keys.onReturnPressed: renameDialog.accept()
            Keys.onEnterPressed: renameDialog.accept()
        }
    }

    AddDeviceDialog {
        id: addDialog
        objectName: "addDialog"
        parent: root.contentItem
    }
}
