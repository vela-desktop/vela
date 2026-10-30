// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

Page {
    CardGroup {
        LinkCard {
            icon: "video-display"
            title: qsTr("Display")
            description: qsTr("Monitors, brightness, resolution, scale")
            onClicked: root.navigate("display")
        }
        LinkCard {
            icon: "audio-speakers"
            title: qsTr("Sound")
            description: qsTr("Volume levels, output, input, sound devices")
            onClicked: root.navigate("sound")
        }
        LinkCard {
            icon: "preferences-desktop-notification"
            title: qsTr("Notifications")
            description: qsTr("Alerts from apps and the system, do not disturb")
            onClicked: root.navigate("notifications")
        }
        LinkCard {
            icon: "preferences-system-power-management"
            title: qsTr("Power")
            description: Status.batteryPresent ? qsTr("Sleep, battery usage, energy saver") : qsTr("Screen and sleep, power mode")
            onClicked: root.navigate("power")
        }
        LinkCard {
            icon: "edit-paste"
            title: qsTr("Clipboard")
            description: qsTr("Clipboard history (Win+V), clear")
            onClicked: root.navigate("clipboard")
        }
        LinkCard {
            icon: "system-software-update"
            title: qsTr("Updates")
            description: qsTr("Check for updates, what's new, update channel")
            onClicked: root.navigate("updates")
        }
        LinkCard {
            icon: "help-about"
            title: qsTr("About")
            description: qsTr("Device specifications, rename your PC, operating system specifications")
            onClicked: root.navigate("about")
        }
    }
}
