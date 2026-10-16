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
T.CheckBox {
    id: control
    property alias usable: control.enabled

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
        implicitWidth: 20
        implicitHeight: 20
        color: control.checked ? (control.down ? Qt.darker(Theme.accentFill, 1.1) : control.hovered ? Theme.accentFillHover : Theme.accentFill)
             : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control)
        border.width: control.checked ? 0 : 1
        border.color: Theme.controlStrokeStrong

        Shape {
            visible: control.checked
            anchors.centerIn: parent
            width: 12
            height: 10
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: Theme.accentText
                strokeWidth: 1.5
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                startX: 1; startY: 5.5
                PathLine { x: 4.5; y: 9 }
                PathLine { x: 11; y: 1 }
            }
        }
        FocusRing { visible: control.visualFocus }
    }

    contentItem: Text {
        text: control.text
        color: Theme.text
        font.pixelSize: Theme.fontBody
        verticalAlignment: Text.AlignVCenter
    }
}
