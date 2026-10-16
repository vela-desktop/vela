// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T

// Windows 11's button: standard (gray), accent, or subtle (no background,
// like the cards' "..." buttons), with text, an icon (`icon.name`, from
// Theme.icons) or both. Qt's button (input, Space and focus, accessibility),
// Vela's look: its color fades between states (Fill). Tab gives it the focus
// and a ring, a click doesn't, so the keyboard stays where it was; Enter
// activates it too, like Space.
T.Button {
    id: control
    property bool accent: false
    property bool subtle: false
    property alias usable: control.enabled

    implicitWidth: Math.max(subtle && text === "" ? 32 : 96, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: 32
    leftPadding: 12
    rightPadding: 12
    spacing: 8
    focusPolicy: Qt.TabFocus
    opacity: enabled ? 1 : 0.45
    icon.width: 16
    icon.height: 16

    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    background: Fill {
        color: control.subtle ? (control.down ? Theme.subtlePressed : control.hovered ? Theme.subtleHover : Theme.subtleRest)
             : control.accent ? (control.hovered ? Theme.accentFillHover : Theme.accentFill)
             : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control)
        border.width: control.subtle ? 0 : 1
        border.color: control.accent ? Qt.rgba(0, 0, 0, 0.1) : Theme.controlStroke
        // Keyboard focus: a ring around it, like Windows. At once, like a
        // disabled look: both must read immediately.
        Rectangle {
            visible: control.visualFocus
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.width: 2
            border.color: Theme.text
        }
    }

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            anchors.centerIn: parent
            spacing: control.spacing
            Image {
                visible: control.icon.name !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: control.icon.width
                height: control.icon.height
                source: control.icon.name !== "" ? Theme.icons + encodeURIComponent(control.icon.name) : ""
                sourceSize: Qt.size(width, height)
            }
            Text {
                visible: control.text !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                color: control.accent ? Theme.accentText : Theme.text
                font.pixelSize: Theme.fontBody
            }
        }
    }
}
