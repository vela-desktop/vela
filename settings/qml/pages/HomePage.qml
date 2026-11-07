// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Bluetooth
import Vela.Controls

// The Windows 11 Home: the device at the top, then the most used cards.
Page {
    // The device: wallpaper thumbnail, name, Rename.
    Item {
        width: parent.width
        height: 140
        Rectangle {
            id: preview
            width: 186
            height: 116
            radius: Theme.radiusCard
            color: "black"
            clip: true
            Image {
                anchors.fill: parent
                anchors.margins: 4
                source: Prefs.wallpaper.startsWith(":") ? "qrc" + Prefs.wallpaper : "file://" + Prefs.wallpaper
                sourceSize: Qt.size(356, 216)
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
        }
        Column {
            anchors { left: preview.right; leftMargin: 20; verticalCenter: preview.verticalCenter }
            spacing: 2
            Text {
                text: About.hostname
                color: Theme.text
                font.pixelSize: Theme.fontSubtitle
                font.weight: Font.DemiBold
            }
            Text {
                text: About.model
                visible: text !== ""
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }
            Text {
                text: qsTr("Rename")
                color: Theme.accentFill
                font.pixelSize: Theme.fontBody
                topPadding: 4
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.navigate("about")
                }
            }
        }
    }

    CardGroup {
        title: qsTr("Recommended settings")
        LinkCard {
            icon: "preferences-desktop-wallpaper"
            title: qsTr("Background")
            description: qsTr("Pictures, colors and photos for your desktop")
            onClicked: root.navigate("background")
        }
        LinkCard {
            icon: "preferences-desktop-color"
            title: qsTr("Colors")
            description: qsTr("Accent color, light or dark mode for apps")
            onClicked: root.navigate("colors")
        }
        LinkCard {
            icon: "video-display"
            title: qsTr("Display")
            description: qsTr("Resolution, scale, refresh rate")
            onClicked: root.navigate("display")
        }
    }

    CardGroup {
        title: qsTr("Bluetooth devices")
        visible: Bluetooth.available || Bluetooth.platformBlocked
        Card {
            icon: "preferences-system-bluetooth"
            title: qsTr("Bluetooth")
            // BlueZ's own Discoverable (docs/bluetooth.md §6.6, rule 2).
            description: !Bluetooth.powered ? qsTr("Off")
                : Bluetooth.adapter.discoverable ? qsTr("Discoverable as \"%1\"").arg(Bluetooth.adapter.name)
                : qsTr("On")
            trailing: Switch {
                checked: Bluetooth.powered
                usable: Bluetooth.active
                onToggled: Bluetooth.setPowered(checked)
            }
        }
        LinkCard {
            icon: "list-add"
            title: qsTr("View all devices")
            onClicked: root.navigate("bluetooth")
        }
    }

    CardGroup {
        title: qsTr("Connection")
        LinkCard {
            icon: Status.networkIconName
            title: Status.networkConnected ? (Status.networkName !== "" ? Status.networkName : qsTr("Connected")) : qsTr("Not connected")
            description: Status.networkConnected ? (Status.networkWireless ? qsTr("Wi-Fi · Connected, secured") : qsTr("Ethernet · Connected")) : qsTr("No Internet connection")
            onClicked: root.navigate("network")
        }
    }
}
