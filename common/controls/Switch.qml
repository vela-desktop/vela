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
    property bool showLabel: true
    property alias usable: control.enabled

    implicitWidth: leftPadding + implicitContentWidth + rightPadding
    implicitHeight: 32
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
        implicitWidth: 40
        implicitHeight: 20
        radius: height / 2
        color: control.checked ? (control.hovered || control.down ? Theme.accentFillHover : Theme.accentFill)
             : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Qt.rgba(0, 0, 0, 0.1))
        border.width: control.checked ? 0 : 1
        border.color: Theme.controlStrokeStrong

        Fill {
            id: knob
            readonly property real size: control.down || control.hovered ? 14 : 12
            width: control.down ? 17 : size
            height: size
            radius: size / 2
            anchors.verticalCenter: parent.verticalCenter
            x: 4 + control.visualPosition * (track.width - width - 8)
            color: control.checked ? Theme.accentText : Theme.textSecondary
            // motion: the knob follows a drag; it slides only on its own.
            Behavior on x { enabled: !control.down && Theme.motion > 0; Eased { duration: Theme.fast } }
            Behavior on width { enabled: Theme.motion > 0; Eased { duration: Theme.fast } }
            Behavior on height { enabled: Theme.motion > 0; Eased { duration: Theme.fast } }
        }
        FocusRing { visible: control.visualFocus }
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
