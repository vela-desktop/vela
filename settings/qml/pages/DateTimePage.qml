// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Time & language > Date & time: the time as it is now, automatic sync and the
// time zone (systemd-timedated).
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
                text: Qt.formatTime(page.now, "HH:mm")
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
