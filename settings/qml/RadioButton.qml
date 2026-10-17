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
T.RadioButton {
    id: control
    property alias usable: control.enabled

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
        color: control.checked ? (control.hovered || control.down ? Theme.accentFillHover : Theme.accentFill)
             : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control)
        border.width: control.checked ? 0 : 1
        border.color: Theme.controlStrokeStrong

        Rectangle {
            visible: control.checked
            anchors.centerIn: parent
            width: control.down ? 6 : control.hovered ? 10 : 8
            height: width
            radius: width / 2
            color: Theme.accentText
            Behavior on width { enabled: Theme.motion > 0; Eased { duration: Theme.fast } }
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
