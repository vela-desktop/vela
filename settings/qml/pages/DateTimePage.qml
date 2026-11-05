// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Time & language > Date & time: the time as it is now, automatic sync, the
// time zone (systemd-timedated) and how Vela writes the time (24-hour or
// 12-hour: the taskbar, the notification center, the lock screen and here).
Page {
    id: page
    property date now: new Date()
    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: page.now = new Date()
    }
    readonly property var zones: DateTime.timezones

    Item {
        width: parent.width
        height: 88
        Column {
            anchors.verticalCenter: parent.verticalCenter
            Text {
                text: Qt.formatTime(page.now, Prefs.timeFormat) // 24-hour or 12-hour, as chosen below
                color: Theme.text
                font.pixelSize: 40
                font.weight: Font.DemiBold
            }
            Text {
                text: {
                    const s = Qt.locale().toString(page.now, "dddd d MMMM yyyy")
                    return s.charAt(0).toUpperCase() + s.slice(1)
                }
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }
        }
    }

    CardGroup {
        Card {
            icon: "chronometer"
            title: qsTr("Set time automatically")
            description: DateTime.canNtp ? qsTr("Sync the clock with a time server on the Internet") : qsTr("No time sync service installed")
            trailing: Switch {
                usable: DateTime.canNtp
                checked: DateTime.ntp
                onToggled: DateTime.setNtp(checked)
            }
        }
        Card {
            icon: "preferences-system-time"
            title: qsTr("Time zone")
            trailing: Choice {
                implicitWidth: 320
                model: page.zones.map(z => ({ text: DateTime.describe(z) }))
                currentIndex: page.zones.indexOf(DateTime.timezone)
                onChosen: index => DateTime.setTimezone(page.zones[index])
            }
        }
        Card {
            id: hours
            icon: "preferences-system-time"
            title: qsTr("Time format")
            description: qsTr("In the taskbar, the notification center, the lock screen and here")
            // Each choice shows a time as it would read: 13:45 or 1:45 PM.
            readonly property date sample: new Date(2026, 0, 14, 13, 45)
            trailing: Choice {
                objectName: "clockHours"
                model: [qsTr("24-hour (%1)").arg(Qt.formatTime(hours.sample, "HH:mm")),
                    qsTr("12-hour (%1)").arg(Qt.formatTime(hours.sample, "h:mm AP"))]
                currentIndex: Prefs.clock24 ? 0 : 1
                onChosen: index => Prefs.clock24 = index === 0
            }
        }
    }

    CardGroup {
        title: qsTr("Related settings")
        LinkCard {
            visible: System.available("datetime")
            icon: "preferences-system-time"
            title: qsTr("Additional clock settings")
            description: qsTr("Set the date and time manually, hardware clock")
            onClicked: System.trigger("datetime")
        }
    }
}
