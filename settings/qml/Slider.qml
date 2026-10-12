// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Windows 11 slider: a thin track, a knob with an accent center.
Item {
    id: slider
    property real from: 0
    property real to: 1
    property real value: 0
    property real stepSize: 0
    // Where the filled part starts: `from`, or the middle of a slider that
    // goes both ways (a balance).
    property real origin: from
    property bool live: true // `moved` while dragging, not only on release
    readonly property bool pressed: mouse.pressed
    signal moved(real value)
    signal released(real value)

    implicitWidth: 200
    implicitHeight: 32
    width: implicitWidth
    height: implicitHeight

    readonly property real ratio: to > from ? Math.max(0, Math.min(1, (value - from) / (to - from))) : 0
    readonly property real originRatio: to > from ? Math.max(0, Math.min(1, (origin - from) / (to - from))) : 0

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        x: 10
        width: parent.width - 20
        height: 4
        radius: 2
        color: Theme.controlStrokeStrong
        opacity: 0.6
    }
    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        x: 10 + (parent.width - 20) * Math.min(slider.ratio, slider.originRatio)
        width: (parent.width - 20) * Math.abs(slider.ratio - slider.originRatio)
        height: 4
        radius: 2
        color: Theme.accentFill
    }
    Rectangle {
        x: 10 + (parent.width - 20) * slider.ratio - width / 2
        anchors.verticalCenter: parent.verticalCenter
        width: 20
        height: 20
        radius: 10
        color: Theme.light ? "#ffffff" : "#454545"
        border.width: 1
        border.color: Theme.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.09)
        Rectangle {
            anchors.centerIn: parent
            width: mouse.pressed ? 10 : mouse.containsMouse ? 14 : 12
            height: width
            radius: width / 2
            color: Theme.accentFill
            Behavior on width { NumberAnimation { duration: Theme.fast } }
        }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        function valueAt(x) {
            let r = Math.max(0, Math.min(1, (x - 10) / (slider.width - 20)))
            let v = slider.from + r * (slider.to - slider.from)
            if (slider.stepSize > 0) {
                v = Math.round(v / slider.stepSize) * slider.stepSize
            }
            return v
        }
        onPressed: mouse => {
            slider.value = valueAt(mouse.x)
            if (slider.live) slider.moved(slider.value)
        }
        onPositionChanged: mouse => {
            if (pressed) {
                slider.value = valueAt(mouse.x)
                if (slider.live) slider.moved(slider.value)
            }
        }
        onReleased: {
            slider.moved(slider.value)
            slider.released(slider.value)
        }
        onWheel: wheel => {
            const step = slider.stepSize > 0 ? slider.stepSize : (slider.to - slider.from) / 50
            slider.value = Math.max(slider.from, Math.min(slider.to, slider.value + (wheel.angleDelta.y > 0 ? step : -step)))
            slider.moved(slider.value)
            slider.released(slider.value)
        }
    }
}
