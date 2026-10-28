// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Bluetooth
import Vela.Bluetooth.Ui
import Vela.Controls

// The paired devices on quick settings' Bluetooth page (docs/bluetooth.md
// §9.2): connected first, with their state and battery. A click opens one,
// as a Wi-Fi network opens, with Connect or Disconnect, or Cancel while it
// connects; another click closes it. Up to 340 units tall, then it
// scrolls.
Flickable {
    id: view
    // The open device's path, empty if none.
    property string expanded: ""
    // Its height once every device has finished opening or closing: the
    // panel goes there at once, not frame by frame.
    readonly property real finalHeight: Math.min(devices.finalHeight, 340)

    height: Math.min(contentHeight, 340)
    contentHeight: devices.height
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    Column {
        id: devices
        width: parent.width
        spacing: 2
        readonly property real finalHeight: {
            let h = height
            for (let i = 0; i < deviceRows.count; ++i) {
                const item = deviceRows.itemAt(i)
                if (item) {
                    h += item.fullHeight - item.height
                }
            }
            return h
        }
        Repeater {
            id: deviceRows
            model: Bluetooth.paired
            delegate: ListItem {
                id: device
                required property var modelData
                readonly property bool open: view.expanded === modelData.path
                readonly property bool busy: modelData.operation !== ""
                readonly property bool cancellable: DeviceState.cancellable(modelData)
                readonly property real fullHeight: open ? 52 + 32 + 12 : 52
                objectName: "device " + modelData.name
                width: devices.width
                height: fullHeight
                highlighted: open
                text: modelData.name
                Accessible.description: DeviceState.description(modelData)
                Behavior on height { enabled: Theme.motion > 0; Eased {} }
                onClicked: view.expanded = open ? "" : modelData.path
                function activate() {
                    if (cancellable) {
                        Bluetooth.cancel(modelData.path)
                    } else if (!busy) {
                        if (modelData.connected) {
                            Bluetooth.disconnectDevice(modelData.path)
                        } else {
                            Bluetooth.connectDevice(modelData.path)
                        }
                    }
                }

                Icon {
                    x: 12
                    y: 16
                    size: 20
                    name: DeviceState.icon(device.modelData)
                }
                Column {
                    x: 44
                    y: (52 - height) / 2
                    width: parent.width - 56
                    Text {
                        width: parent.width
                        text: device.modelData.name
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    Text {
                        objectName: "state"
                        width: parent.width
                        text: DeviceState.description(device.modelData)
                        textFormat: Text.PlainText
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                }
                // Shown while open, and while it closes; a click there
                // doesn't close it.
                MouseArea {
                    y: 52
                    width: parent.width
                    height: parent.height - 52
                    clip: true
                    Button {
                        objectName: "action"
                        visible: device.open || device.height > 52
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        width: 120
                        accent: !device.modelData.connected && !device.cancellable
                        usable: Bluetooth.active && (!device.busy || device.cancellable)
                        text: device.cancellable ? qsTr("Cancel")
                            : device.modelData.connected ? qsTr("Disconnect") : qsTr("Connect")
                        onClicked: device.activate()
                    }
                }
            }
        }
    }
}
