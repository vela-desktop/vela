// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T

// The Windows 11 toggle switch, with "On"/"Off" on the left (`showLabel`).
// Qt's switch: a click, Space or a drag of the knob toggles it and emits
// `toggled()`, and a binding on `checked` survives that (the backend's value
// stays the truth). The track's color fades; the knob follows a drag
// directly and slides when released or toggled, unless animation effects are
// off, where it jumps.
T.Switch {
    id: control
    property string styleVariant: Theme.controlsVariant
    readonly property bool expressive: styleVariant === "expressive"
    property bool showLabel: !expressive
    property alias usable: control.enabled

    implicitWidth: leftPadding + implicitContentWidth + rightPadding
    implicitHeight: expressive ? 40 : 32
    padding: 0
    rightPadding: track.width + (showLabel ? spacing : 0)
    spacing: 12
    focusPolicy: Qt.TabFocus
    hoverEnabled: true // deterministic hover for Qt Quick and headless tests
    opacity: enabled ? 1 : 0.45

    indicator: Fill {
        id: track
        x: control.width - width
        anchors.verticalCenter: parent.verticalCenter
        // Expressive: M3's switch (track 52 × 32, outline 2, unselected
        // track on surface container highest).
        implicitWidth: control.expressive ? 52 : 40
        implicitHeight: control.expressive ? 32 : 20
        radius: height / 2
        color: control.expressive ? (control.checked ? Theme.primary : Theme.surfaceContainerHighest)
             : control.checked ? (control.hovered || control.down ? Theme.accentFillHover : Theme.accentFill)
             : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Qt.rgba(0, 0, 0, 0.1))
        border.width: control.expressive ? (control.checked ? 0 : 2) : control.checked ? 0 : 1
        border.color: control.expressive ? Theme.outline : Theme.controlStrokeStrong

        Fill {
            id: knob
            z: 1 // above the state layer
            // Expressive: M3's handle, 16 off, 24 on, 28 pressed; the hover
            // is the state layer above. Fluent: WinUI's, larger hovered.
            readonly property real size: control.expressive ? (control.down ? 28 : control.checked ? 24 : 16)
                : control.down || control.hovered ? 14 : 12
            width: control.expressive ? size : control.down ? 17 : size
            height: size
            radius: size / 2
            anchors.verticalCenter: parent.verticalCenter
            // Expressive: the handle's center keeps 16 from the track's
            // ends (M3's 16 handle sits 8 inside a 32 track), whatever its
            // size. Fluent: 4 from the edges.
            x: control.expressive ? 16 + control.visualPosition * (track.width - 32) - width / 2
                                  : 4 + control.visualPosition * (track.width - width - 8)
            color: control.expressive ? (control.checked ? Theme.primaryText : Theme.outline)
                : control.checked ? Theme.accentText : Theme.textSecondary
            // motion: the knob follows a drag; it slides only on its own,
            // expressive on M3's fast spatial curve (Spatial; the default
            // motion is Eased in `fast`, as before).
            Behavior on x { enabled: !control.down && Theme.motion > 0; Spatial { speed: "fast"; rest: Theme.fast } }
            Behavior on width { enabled: Theme.motion > 0; Spatial { speed: "fast"; rest: Theme.fast } }
            Behavior on height { enabled: Theme.motion > 0; Spatial { speed: "fast"; rest: Theme.fast } }
        }
        FocusRing { z: 2; visible: control.visualFocus; expressive: control.expressive }
        // Expressive: M3's state layer, a 40 circle around the handle, in
        // the handle's role (primary when on, on surface when off): 8%
        // hovered, 10% pressed.
        Fill {
            id: halo
            // Below the handle (its z); declared last, as tests find the
            // handle and the ring by index.
            visible: control.expressive
            readonly property color tint: control.checked ? Theme.primary : Theme.surfaceText
            readonly property real amount: control.down ? Theme.statePressed : control.hovered ? Theme.stateHover : 0
            width: 40
            height: 40
            radius: 20
            x: knob.x + knob.width / 2 - 20
            anchors.verticalCenter: parent.verticalCenter
            color: Qt.rgba(tint.r, tint.g, tint.b, amount)
        }
    }

    // As wide as the longer word, so the switch doesn't move when it toggles.
    contentItem: Item {
        readonly property alias text: label.text
        visible: control.showLabel
        implicitWidth: control.showLabel ? Math.max(onMetrics.width, offMetrics.width) : 0
        implicitHeight: label.implicitHeight
        Text {
            id: label
            anchors.fill: parent
            text: control.checked ? qsTr("On") : qsTr("Off")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignRight
        }
        TextMetrics { id: onMetrics; text: qsTr("On"); font.pixelSize: Theme.fontBody }
        TextMetrics { id: offMetrics; text: qsTr("Off"); font.pixelSize: Theme.fontBody }
    }
}
