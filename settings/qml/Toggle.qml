// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Windows 11 toggle switch, with "On"/"Off" on the left.
Item {
    id: toggle
    property bool checked: false
    property bool showLabel: true
    property bool usable: true
    signal toggled(bool checked)

    width: (showLabel ? label.width + 12 : 0) + 40
    height: 32
    opacity: usable ? 1 : 0.45

    Text {
        id: label
        visible: toggle.showLabel
        anchors.verticalCenter: parent.verticalCenter
        text: toggle.checked ? qsTr("On") : qsTr("Off")
        color: Theme.text
        font.pixelSize: Theme.fontBody
        width: Math.max(implicitWidth, metrics.width)
        horizontalAlignment: Text.AlignRight
        TextMetrics { id: metrics; text: qsTr("Off"); font.pixelSize: Theme.fontBody }
    }
    Rectangle {
        id: track
        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
        width: 40
        height: 20
        radius: 10
        color: toggle.checked ? (mouse.containsMouse ? Theme.accentFillHover : Theme.accentFill)
                              : (mouse.containsMouse ? Theme.controlHover : Qt.rgba(0, 0, 0, 0.1))
        border.width: toggle.checked ? 0 : 1
        border.color: Theme.controlStrokeStrong
        Rectangle {
            id: knob
            readonly property real size: mouse.pressed ? 14 : mouse.containsMouse ? 14 : 12
            width: mouse.pressed ? 17 : size
            height: size
            radius: size / 2
            anchors.verticalCenter: parent.verticalCenter
            x: toggle.checked ? parent.width - width - 4 : 4
            color: toggle.checked ? Theme.accentText : Theme.textSecondary
            Behavior on x { enabled: Theme.motion > 0; NumberAnimation { duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate } }
            Behavior on width { NumberAnimation { duration: Theme.fast } }
        }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: toggle.usable
        onClicked: {
            toggle.checked = !toggle.checked
            toggle.toggled(toggle.checked)
        }
    }
}
