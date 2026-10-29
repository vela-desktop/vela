// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The recording bar (created by snip.cpp while wf-recorder runs): a red dot,
// the time and a Stop button. The same shortcut as the Snipping Tool stops too.
Window {
    visible: false
    width: row.width + 24
    height: 44
    color: "transparent"

    function clock(s) {
        const m = Math.floor(s / 60)
        const r = s % 60
        return m + ":" + (r < 10 ? "0" : "") + r
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusOverlay
        color: Theme.dialog
        border.width: 1
        border.color: Theme.stroke
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 10
            Rectangle { width: 10; height: 10; radius: 5; color: "#e5484d"; anchors.verticalCenter: parent.verticalCenter }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: clock(Snip.recordSeconds)
                color: Theme.text
                font.pixelSize: Theme.fontBody
                font.features: { "tnum": 1 }
            }
            Rectangle {
                width: stopText.width + 24
                height: 32
                radius: Theme.radius
                color: stopMouse.containsMouse ? Theme.subtleHover : "transparent"
                border.width: 1
                border.color: Theme.stroke
                Text { id: stopText; anchors.centerIn: parent; text: qsTr("Stop"); color: Theme.text; font.pixelSize: Theme.fontBody }
                MouseArea { id: stopMouse; anchors.fill: parent; hoverEnabled: true; onClicked: Snip.stopRecording() }
            }
        }
    }
}
