// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T

// The Windows 11 slider: a thin track, a knob with an accent center that
// grows under the pointer and shrinks when pressed. Qt's slider: dragging,
// clicking on the track, arrows, Page Up/Down, Home/End and the wheel, and a
// binding on `value` survives them. `moved()` while it changes from input
// (with `live: false` the value changes on release); `released(value)` once
// the user is done (released, a key, a notch of the wheel). The value follows
// the input and is never animated (docs/visual-coherence.md, invariant 6).
T.Slider {
    id: control
    // Where the filled part starts: `from`, or the middle of a slider that
    // goes both ways (a balance).
    property real origin: from
    // Where a double click on the knob puts it back (a balance's middle), as
    // `released(resetValue)`; NaN: nowhere.
    property real resetValue: NaN
    // A tick under the track at a value worth finding again (the suggested
    // one); NaN: none.
    property real mark: NaN
    property alias usable: control.enabled
    signal released(real value)

    implicitWidth: 200
    implicitHeight: 32
    padding: 0
    focusPolicy: Qt.TabFocus
    wheelEnabled: true
    opacity: enabled ? 1 : 0.45

    // The knob's center travels between these, as Qt maps the pointer.
    readonly property real trackStart: leftPadding + handle.width / 2
    readonly property real trackLength: availableWidth - handle.width

    onPressedChanged: {
        if (!pressed) {
            released(d.resetting ? resetValue : value)
            d.resetting = false
        }
    }
    // The wheel moves it without pressing it.
    onMoved: if (!pressed) released(value)

    function ratioOf(v) {
        return to > from ? Math.max(0, Math.min(1, (v - from) / (to - from))) : 0
    }

    QtObject {
        id: d
        property bool resetting: false
        property double pressedAt: 0
    }
    // A second press on the knob soon after the first: a double click. Qt's
    // slider has no signal for it, and a TapHandler would take the press
    // from it; a PointHandler only watches.
    PointHandler {
        onActiveChanged: {
            if (!active)
                return
            const center = control.handle.x + control.handle.width / 2
            const onKnob = Math.abs(point.position.x - center) <= control.handle.width / 2
            const now = Date.now()
            d.resetting = !isNaN(control.resetValue) && onKnob
                && now - d.pressedAt < Qt.styleHints.mouseDoubleClickInterval
            d.pressedAt = onKnob && !d.resetting ? now : 0
        }
    }

    background: Item {
        implicitWidth: 200
        implicitHeight: 32
        Rectangle {
            x: control.trackStart
            anchors.verticalCenter: parent.verticalCenter
            width: control.trackLength
            height: 4
            radius: 2
            color: Theme.controlStrokeStrong
            opacity: 0.6
        }
        Rectangle {
            visible: !isNaN(control.mark) && control.to > control.from
            x: control.trackStart + control.trackLength * control.ratioOf(control.mark) - width / 2
            y: parent.height / 2 + 6
            width: 1
            height: 5
            color: Theme.textTertiary
        }
        Rectangle {
            readonly property real originRatio: control.ratioOf(control.origin)
            readonly property real position: control.visualPosition
            x: control.trackStart + control.trackLength * Math.min(position, originRatio)
            anchors.verticalCenter: parent.verticalCenter
            width: control.trackLength * Math.abs(position - originRatio)
            height: 4
            radius: 2
            color: Theme.accentFill
        }
    }

    handle: Rectangle {
        x: control.trackStart + control.visualPosition * control.trackLength - width / 2
        anchors.verticalCenter: parent.verticalCenter
        implicitWidth: 20
        implicitHeight: 20
        radius: 10
        color: Theme.light ? "#ffffff" : "#454545"
        border.width: 1
        border.color: Theme.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.09)
        Rectangle {
            anchors.centerIn: parent
            width: control.pressed ? 10 : control.hovered ? 14 : 12
            height: width
            radius: width / 2
            color: Theme.accentFill
            Behavior on width { NumberAnimation { duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate } }
        }
        // Keyboard focus: a ring around the knob, like Windows.
        Rectangle {
            visible: control.visualFocus
            anchors.fill: parent
            anchors.margins: -3
            radius: width / 2
            color: "transparent"
            border.width: 2
            border.color: Theme.text
        }
    }
}
