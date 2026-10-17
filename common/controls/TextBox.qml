// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic as C

// Windows 11's text box: the accent line at the bottom when it has the focus.
// With `password: true` it shows dots and can't be copied.
C.TextField {
    id: field
    property bool password: false
    implicitWidth: 240
    implicitHeight: 32
    color: Theme.text
    placeholderTextColor: Theme.textTertiary
    selectionColor: Theme.accent
    selectedTextColor: "white"
    font.pixelSize: Theme.fontBody
    leftPadding: 11
    rightPadding: 11
    verticalAlignment: TextInput.AlignVCenter
    echoMode: password ? TextInput.Password : TextInput.Normal
    passwordCharacter: "●"
    inputMethodHints: password ? (Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText) : Qt.ImhNone
    background: Fill {
        color: field.activeFocus ? (Theme.light ? "#ffffff" : Qt.rgba(0.12, 0.12, 0.12, 0.7)) : field.hovered ? Theme.controlHover : Theme.control
        border.width: 1
        border.color: Theme.controlStroke
        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; leftMargin: 1; rightMargin: 1 }
            height: field.activeFocus ? 2 : 1
            radius: 1
            color: field.activeFocus ? Theme.accentFill : Theme.controlStrokeStrong
            opacity: field.activeFocus ? 1 : 0.6
        }
    }
}
