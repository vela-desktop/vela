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
    property string styleVariant: Theme.controlsVariant
    readonly property bool expressive: styleVariant === "expressive"
    // The expressive track and bar knob; a thick slider keeps its own shape
    // (a quick settings style chose it) and takes only the tonal colors.
    readonly property bool expressiveShape: expressive && !thick
    // The filled part and the knob's center: the accent, or gray for a muted
    // volume (Quick Settings).
    property color fillColor: expressive ? Theme.primary : Theme.accentFill
    // A thick track and a wide knob without the center (a quick settings
    // style's, WindowGlass's): the track spans the whole width, filled up to
    // the knob's middle.
    property bool thick: false
    // The knob's edge (a style without borders turns it off).
    property bool knobBorder: true
    property alias usable: control.enabled
    signal released(real value)

    implicitWidth: 200
    implicitHeight: expressiveShape ? 40 : 32
    padding: 0
    focusPolicy: Qt.TabFocus
    hoverEnabled: true // do not inherit platform-dependent hover defaults
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
            x: control.thick ? 0 : control.trackStart
            anchors.verticalCenter: parent.verticalCenter
            width: control.thick ? control.availableWidth : control.trackLength
            height: control.expressiveShape ? 12 : control.thick ? 10 : 4
            radius: height / 2
            // Expressive: M3's inactive track is secondary container.
            color: control.expressive ? Theme.secondaryContainer : Theme.controlStrokeStrong
            opacity: control.expressive ? 1 : 0.6
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
            // Thick: from the track's start to the knob's middle.
            readonly property real start: control.thick && originRatio === 0 ? 0
                : control.trackStart + control.trackLength * Math.min(position, originRatio)
            readonly property real end: control.trackStart + control.trackLength * Math.max(position, originRatio)
            x: start
            anchors.verticalCenter: parent.verticalCenter
            width: end - start
            height: control.expressiveShape ? 12 : control.thick ? 10 : 4
            radius: height / 2
            color: control.fillColor
        }
    }

    handle: Rectangle {
        x: control.trackStart + control.visualPosition * control.trackLength - width / 2
        anchors.verticalCenter: parent.verticalCenter
        implicitWidth: control.expressiveShape ? 8 : control.thick ? 40 : 20
        implicitHeight: control.expressiveShape ? 28 : control.thick ? 25 : 20
        radius: control.expressiveShape ? Math.min(Theme.shapeSmall, height / 2) : control.thick ? 12 : 10
        color: control.expressiveShape ? Theme.primary : control.thick ? Theme.popup : Theme.light ? "#ffffff" : "#454545"
        border.width: control.expressiveShape ? 0 : control.knobBorder ? 1 : 0
        border.color: control.expressive ? Theme.outlineVariant : Theme.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.09)
        Rectangle {
            visible: !control.thick && !control.expressiveShape
            anchors.centerIn: parent
            width: control.pressed ? 10 : control.hovered ? 14 : 12
            height: width
            radius: width / 2
            color: control.fillColor
            Behavior on width { enabled: Theme.motion > 0; Eased { duration: Theme.fast } }
        }
        FocusRing { visible: control.visualFocus }
    }
}
