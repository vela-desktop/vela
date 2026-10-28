// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Bluetooth
import Vela.Controls

// On the Bluetooth page (docs/bluetooth.md §9.1): the PC stayed visible
// after Vela's search because another app is searching (§6.6, rule 3).
// Only the user's "Stop being visible" ends it.
Card {
    objectName: "stillVisible"
    visible: Bluetooth.available && Bluetooth.stillVisible
    icon: "view-visible"
    title: qsTr("Your PC is still visible to nearby devices")
    description: qsTr("Another app is searching for devices.")
    trailing: Button {
        objectName: "stopBeingVisible"
        text: qsTr("Stop being visible")
        usable: Bluetooth.active
        onClicked: Bluetooth.stopBeingVisible()
    }
}
