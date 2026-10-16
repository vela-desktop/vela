// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// System > Display > Night light, like Windows 11: turn it on now, how warm it
// should be, and the schedule (sunset to sunrise, with the time zone's sun
// times, or between two times).
Page {
    id: page
    // Times every quarter hour, to choose when to turn it on and off.
    readonly property var times: {
        const list = []
        for (let m = 0; m < 24 * 60; m += 15) {
            list.push(String(Math.floor(m / 60)).padStart(2, "0") + ":" + String(m % 60).padStart(2, "0"))
        }
        return list
    }

    // A single-choice option (radio button).
    component Radio: Item {
        id: radio
        property string text
        property bool checked
        property bool usable: true
        signal clicked()
        width: parent.width
        height: 32
        opacity: usable ? 1 : 0.45
        Rectangle {
            id: dot
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            radius: 10
            color: radio.checked ? Theme.accentFill : Theme.control
            border.width: radio.checked ? 0 : 1
            border.color: Theme.controlStrokeStrong
            Rectangle {
                visible: radio.checked
                anchors.centerIn: parent
                width: radioMouse.containsMouse ? 10 : 8
                height: width
                radius: width / 2
                color: Theme.accentText
            }
        }
        Text {
            anchors { left: dot.right; leftMargin: 12; verticalCenter: parent.verticalCenter }
            text: radio.text
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        MouseArea {
            id: radioMouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: radio.usable
            onClicked: radio.clicked()
        }
    }

    CardGroup {
        Card {
            icon: "redshift-status-on"
            title: qsTr("Night light")
            description: qsTr("Warmer colors in the evening, to ease eye strain and help you sleep")
            trailing: Button {
                text: Prefs.nightLight ? qsTr("Turn off now") : qsTr("Turn on now")
                onClicked: Prefs.nightLight = !Prefs.nightLight
            }
        }
        Card {
            icon: "color-management"
            title: qsTr("Strength")
            trailing: Slider {
                width: 220
                from: 0
                to: 100
                stepSize: 1
                live: false
                value: Prefs.nightStrength
                onReleased: value => Prefs.nightStrength = Math.round(value)
            }
        }
        Card {
            icon: "applications-graphics"
            title: qsTr("Experimental NVIDIA fix")
            description: qsTr("Use Vulkan instead of the monitor's gamma LUT if Night light causes broken colors. May disable direct scanout.")
            trailing: Switch {
                checked: Prefs.nightNvidiaFix
                onToggled: Prefs.nightNvidiaFix = checked
            }
        }
        Card {
            icon: "chronometer"
            title: qsTr("Schedule night light")
            trailing: Switch {
                checked: Prefs.nightSchedule !== "no"
                onToggled: Prefs.nightSchedule = checked ? (Prefs.sunset !== "" ? "sunset" : "hours") : "no"
            }
            contentItem: Column {
                visible: Prefs.nightSchedule !== "no"
                x: 36
                width: parent.width - 36
                spacing: 4
                Radio {
                    text: qsTr("Sunset to sunrise") + (Prefs.sunset !== "" ? " (" + Prefs.sunset + " - " + Prefs.sunrise + ")" : "")
                    usable: Prefs.sunset !== ""
                    checked: Prefs.nightSchedule === "sunset"
                    onClicked: Prefs.nightSchedule = "sunset"
                }
                Radio {
                    text: qsTr("Set hours")
                    checked: Prefs.nightSchedule === "hours"
                    onClicked: Prefs.nightSchedule = "hours"
                }
                Row {
                    visible: Prefs.nightSchedule === "hours"
                    x: 32
                    spacing: 12
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Turn on")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Choice {
                        model: page.times
                        currentIndex: Math.max(0, page.times.indexOf(Prefs.nightFrom))
                        onChosen: index => Prefs.nightFrom = page.times[index]
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Turn off")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Choice {
                        model: page.times
                        currentIndex: Math.max(0, page.times.indexOf(Prefs.nightTo))
                        onChosen: index => Prefs.nightTo = page.times[index]
                    }
                }
            }
        }
    }
}
