// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// System > Power: when to turn off the screen, locking, the power saving mode
// (power-profiles-daemon), the battery.
Page {
    id: page
    readonly property var minutes: [1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 240, 300, 0]
    function label(m) {
        if (m === 0) return qsTr("Never")
        if (m < 60) return m + (m === 1 ? qsTr(" minute") : qsTr(" minutes"))
        return (m / 60) + (m === 60 ? qsTr(" hour") : qsTr(" hours"))
    }
    readonly property var profileNames: ({ "power-saver": qsTr("Energy saver"), "balanced": qsTr("Balanced"), "performance": qsTr("Best performance") })
    readonly property var profiles: ["power-saver", "balanced", "performance"].filter(p => Status.powerProfiles.indexOf(p) >= 0)

    Card {
        visible: Status.batteryPresent
        icon: Status.batteryCharging ? "battery-good-charging" : "battery-good"
        title: Status.batteryPercent + "%"
        description: Status.batteryCharging ? qsTr("Charging") : qsTr("On battery")
        minimumHeight: 80
    }

    CardGroup {
        title: qsTr("Power")
        Card {
            icon: "video-display"
            title: qsTr("Screen and sleep")
            description: qsTr("After this long without using the mouse or keyboard, the screen turns off")
            trailing: Choice {
                model: page.minutes.map(m => page.label(m))
                currentIndex: Math.max(0, page.minutes.indexOf(Prefs.screenOffMinutes))
                onChosen: index => Prefs.screenOffMinutes = page.minutes[index]
            }
        }
        Card {
            icon: "system-lock-screen"
            title: qsTr("Lock the screen when it turns off")
            description: qsTr("You'll need your password to get back in")
            trailing: Switch {
                checked: Prefs.lockOnIdle
                onToggled: Prefs.lockOnIdle = checked
            }
        }
        Card {
            visible: page.profiles.length > 1
            icon: "battery-profile-performance"
            title: qsTr("Power mode")
            description: qsTr("Optimize your device based on power use and performance")
            trailing: Choice {
                model: page.profiles.map(p => page.profileNames[p])
                currentIndex: page.profiles.indexOf(Status.powerProfile)
                onChosen: index => Status.powerProfile = page.profiles[index]
            }
        }
    }
}
