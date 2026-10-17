// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// System > Notifications.
Page {
    CardGroup {
        Card {
            icon: "preferences-desktop-notification"
            title: qsTr("Notifications")
            description: qsTr("Alerts from apps and the system, in the bottom-right corner and in the notification center (Win+N)")
        }
        Card {
            icon: "notifications-disabled"
            title: qsTr("Do not disturb")
            description: qsTr("Notifications go straight to the notification center, without popping up")
            trailing: Switch {
                checked: Prefs.doNotDisturb
                onToggled: Prefs.doNotDisturb = checked
            }
        }
    }
}
