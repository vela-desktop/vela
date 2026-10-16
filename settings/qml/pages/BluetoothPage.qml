// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Bluetooth & devices: the switch, "Add device" and the paired devices, like
// Windows 11.
Page {
    id: page
    Component.onCompleted: Bluetooth.refresh()
    Component.onDestruction: Bluetooth.stopDiscovery()

    function deviceIcon(icon) {
        // BlueZ icons (audio-headset, input-mouse...) are already theme names.
        return icon !== "" ? icon : "preferences-system-bluetooth"
    }
    function status(d) {
        if (d.busy) return qsTr("Connecting...")
        let s = d.connected ? qsTr("Connected") : qsTr("Paired")
        if (d.battery >= 0) s += qsTr(" · Battery ") + d.battery + "%"
        return s
    }

    Card {
        visible: !Bluetooth.available
        icon: "preferences-system-bluetooth-inactive"
        title: qsTr("Bluetooth isn't available")
        description: qsTr("There's no Bluetooth adapter, or the bluetooth service isn't running.")
    }

    Card {
        visible: Bluetooth.available
        icon: Bluetooth.powered ? "preferences-system-bluetooth" : "preferences-system-bluetooth-inactive"
        title: qsTr("Bluetooth")
        description: Bluetooth.powered ? qsTr("Discoverable as \"") + Bluetooth.adapterName + "\"" : qsTr("Off")
        minimumHeight: 80
        trailing: Switch {
            checked: Bluetooth.powered
            onToggled: Bluetooth.powered = checked
        }
    }

    Card {
        visible: Bluetooth.available
        icon: "list-add"
        title: qsTr("Devices")
        description: qsTr("Mouse, keyboard, pen, audio, displays and docks, other devices")
        trailing: Button {
            text: qsTr("Add device")
            usable: Bluetooth.powered
            onClicked: {
                addDialog.open()
                Bluetooth.startDiscovery()
            }
        }
    }

    Text {
        visible: Bluetooth.error !== ""
        width: parent.width
        text: Bluetooth.error
        color: Theme.critical
        font.pixelSize: Theme.fontBody
        wrapMode: Text.Wrap
        topPadding: 8
        bottomPadding: 8
    }

    Repeater {
        model: Bluetooth.paired
        delegate: Card {
            id: deviceCard
            required property var modelData
            icon: page.deviceIcon(modelData.icon)
            title: modelData.name
            description: page.status(modelData)
            trailing: Row {
                spacing: 8
                Button {
                    text: deviceCard.modelData.connected ? qsTr("Disconnect") : qsTr("Connect")
                    usable: !deviceCard.modelData.busy && Bluetooth.powered
                    onClicked: deviceCard.modelData.connected ? Bluetooth.disconnectDevice(deviceCard.modelData.path)
                                                              : Bluetooth.connectDevice(deviceCard.modelData.path)
                }
                Button {
                    subtle: true
                    text: qsTr("Remove")
                    onClicked: {
                        removeDialog.device = deviceCard.modelData
                        removeDialog.open()
                    }
                }
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
        parent: root.contentItem
        property var device: null
        title: qsTr("Remove this device?")
        buttons: [{ text: qsTr("Yes"), accent: true, value: "yes" }, { text: qsTr("No"), value: "cancel" }]
        onChosen: value => { if (value === "yes") Bluetooth.removeDevice(device.path) }
        Text {
            width: parent.width
            text: removeDialog.device ? qsTr("To use ") + removeDialog.device.name + qsTr(" again, you'll need to pair it once more.") : ""
            color: Theme.text
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
    }

    // "Add a device": the search, and a click to pair.
    Dialog {
        id: addDialog
        parent: root.contentItem
        title: qsTr("Add a device")
        buttons: [{ text: qsTr("Cancel"), value: "cancel" }]
        dialogWidth: 520
        onChosen: Bluetooth.stopDiscovery()
        Column {
            width: parent.width
            spacing: 8
            Text {
                width: parent.width
                text: qsTr("Make sure your device is turned on and discoverable. Select it below to connect.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
            }
            Item {
                width: parent.width
                height: Math.min(320, Math.max(found.contentHeight, 80))
                ListView {
                    id: found
                    anchors.fill: parent
                    clip: true
                    spacing: 2
                    model: Bluetooth.found
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: Rectangle {
                        id: candidate
                        required property var modelData
                        width: found.width
                        height: 56
                        radius: Theme.radius
                        color: candidateMouse.containsMouse ? Theme.subtleHover : "transparent"
                        Image {
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: 24
                            height: 24
                            source: Theme.icons + encodeURIComponent(page.deviceIcon(candidate.modelData.icon))
                            sourceSize: Qt.size(width, height)
                        }
                        Column {
                            x: 52
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                text: candidate.modelData.name
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                            }
                            Text {
                                visible: candidate.modelData.busy
                                text: qsTr("Pairing...")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                        MouseArea {
                            id: candidateMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: !candidate.modelData.busy
                            onClicked: Bluetooth.pairAndConnect(candidate.modelData.path)
                        }
                    }
                }
                Text {
                    visible: found.count === 0
                    anchors.centerIn: parent
                    text: Bluetooth.discovering ? qsTr("Looking for devices...") : qsTr("No devices found")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }
            }
        }
    }
    // Paired: the device moves from "found" to paired and the dialog closes.
    Connections {
        target: Bluetooth
        property int pairedCount: Bluetooth.paired.length
        function onChanged() {
            if (addDialog.visible && !addDialog.closing && Bluetooth.paired.length > pairedCount) {
                addDialog.close()
                Bluetooth.stopDiscovery()
            }
            pairedCount = Bluetooth.paired.length
        }
    }
}
