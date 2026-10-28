// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Bluetooth
import Vela.Controls
import "bluetooth.js" as State

// A paired device (docs/bluetooth.md §9.1): its icon, name, state and
// battery, Cancel while a step can be stopped, and a menu with Connect or
// Disconnect, Rename, Remove and, for a headset with sound, "Sound
// settings".
Card {
    id: card
    property var device: ({})
    // The device has a sound card (vela-audio): its page has the codec and
    // profile.
    property bool hasSound: false
    signal renameAsked()
    signal removeAsked()
    signal soundAsked()

    readonly property bool busy: (device.operation || "") !== ""
    icon: State.icon(device)
    title: device.name || ""
    description: State.description(device)

    trailing: Row {
        spacing: 8
        Button {
            objectName: "cancel"
            visible: State.cancellable(card.device)
            text: qsTr("Cancel")
            onClicked: Bluetooth.cancel(card.device.path)
        }
        MoreMenu {
            objectName: "more"
            usable: !card.busy && Bluetooth.active
            entries: {
                const list = []
                if (card.device.connected)
                    list.push({ text: qsTr("Disconnect"), action: () => Bluetooth.disconnectDevice(card.device.path) })
                else
                    list.push({ text: qsTr("Connect"), action: () => Bluetooth.connectDevice(card.device.path) })
                list.push({ text: qsTr("Rename"), action: () => card.renameAsked() })
                list.push({ text: qsTr("Remove"), action: () => card.removeAsked() })
                if (card.hasSound)
                    list.push({ text: qsTr("Sound settings"), action: () => card.soundAsked() })
                return list
            }
        }
    }
}
