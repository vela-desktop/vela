// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes
import QtQuick.Templates as T
import Vela.Controls

// Windows 11's check box, with its label on the right. Qt's check box: a
// click or Space toggles it and emits `toggled()`, and a binding on `checked`
// survives that. The box's color fades between states (Fill); the check mark
// shows at once.
//
// Expressive: M3's checkbox: an 18 box with 2 corners, outlined (2) in on
// surface variant, filled in primary with the mark in on primary when
// checked, and a 40 state layer around it (on surface, primary when
// checked). The row stays 22, Vela's density for Properties: the layer
// reaches past it, as M3's reaches past the box.
T.CheckBox {
    id: control
    property alias usable: control.enabled
    property string styleVariant: Theme.controlsVariant
    readonly property bool expressive: styleVariant === "expressive"

    implicitWidth: leftPadding + implicitContentWidth + rightPadding
    implicitHeight: 22 // the rows of Properties
    padding: 0
    leftPadding: box.width + spacing
    spacing: 8
    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    opacity: enabled ? 1 : 0.45

    indicator: Fill {
        id: box
        anchors.verticalCenter: parent.verticalCenter
        implicitWidth: control.expressive ? 18 : 20
        implicitHeight: control.expressive ? 18 : 20
        radius: control.expressive ? 2 : Theme.radius
        color: control.expressive ? (control.checked ? Theme.primary : Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, 0))
             : control.checked ? (control.down ? Qt.darker(Theme.accentFill, 1.1) : control.hovered ? Theme.accentFillHover : Theme.accentFill)
             : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control)
        border.width: control.checked ? 0 : control.expressive ? 2 : 1
        border.color: control.expressive ? Theme.surfaceVariantText : Theme.controlStrokeStrong

        Shape {
            visible: control.checked
            anchors.centerIn: parent
            width: 12
            height: 10
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: control.expressive ? Theme.primaryText : Theme.accentText
                strokeWidth: control.expressive ? 2 : 1.5
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                startX: 1; startY: 5.5
                PathLine { x: 4.5; y: 9 }
                PathLine { x: 11; y: 1 }
            }
        }
        FocusRing { visible: control.visualFocus; expressive: control.expressive }
        // Expressive: the state layer (declared last: tests find the mark
        // and the ring by index).
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
