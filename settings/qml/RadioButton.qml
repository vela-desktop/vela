// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
import Vela.Controls

// Windows 11's radio button, with its label on the right. Qt's radio button:
// a click or Space chooses it, the others in its parent let go, and a binding
// on `checked` survives that. The circle's color fades between states (Fill);
// the dot grows on hover and shrinks while pressed, unless animation effects
// are off.
//
// Expressive: M3's radio button: a 20 ring (2) in on surface variant, in
// primary with a 10 dot when chosen, and a 40 state layer around it (on
// surface, primary when chosen). The row stays 32, Vela's density.
T.RadioButton {
    id: control
    property alias usable: control.enabled
    property string styleVariant: Theme.controlsVariant
    readonly property bool expressive: styleVariant === "expressive"

    implicitWidth: leftPadding + implicitContentWidth + rightPadding
    implicitHeight: 32
    padding: 0
    leftPadding: circle.width + spacing
    spacing: 12
    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    opacity: enabled ? 1 : 0.45

    indicator: Fill {
        id: circle
        anchors.verticalCenter: parent.verticalCenter
        implicitWidth: 20
        implicitHeight: 20
        radius: width / 2
        color: control.expressive ? Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, 0)
             : control.checked ? (control.hovered || control.down ? Theme.accentFillHover : Theme.accentFill)
             : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control)
        border.width: control.expressive ? 2 : control.checked ? 0 : 1
        border.color: control.expressive ? (control.checked ? Theme.primary : Theme.surfaceVariantText) : Theme.controlStrokeStrong

        Rectangle {
            visible: control.checked
            anchors.centerIn: parent
            width: control.expressive ? 10 : control.down ? 6 : control.hovered ? 10 : 8
            height: width
            radius: width / 2
            color: control.expressive ? Theme.primary : Theme.accentText
            Behavior on width { enabled: Theme.motion > 0; Eased { duration: Theme.fast } }
        }
        FocusRing { visible: control.visualFocus; expressive: control.expressive }
        // Expressive: the state layer, below the ring (declared last: tests
        // find the dot and the focus ring by index).
        Fill {
            objectName: "stateLayer"
            visible: control.expressive
            z: -1
            readonly property color tint: control.checked ? Theme.primary : Theme.surfaceText
            readonly property real amount: control.down ? Theme.statePressed : control.hovered ? Theme.stateHover : 0
            anchors.centerIn: parent
            width: 40
            height: 40
            radius: 20
            color: Qt.rgba(tint.r, tint.g, tint.b, amount)
        }
    }

    contentItem: Text {
        text: control.text
        color: Theme.text
        font.pixelSize: Theme.fontBody
        verticalAlignment: Text.AlignVCenter
    }
}
