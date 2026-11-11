// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic as C

// Windows 11's text box: the accent line at the bottom when it has the focus.
// With `password: true` it shows dots and can't be copied.
//
// Expressive: M3's filled text field: surface container highest with
// extra small top corners and square bottom ones, a state layer of on
// surface when hovered, the active indicator at the bottom (1 in on surface
// variant, 2 in primary with the focus), 16 on each side. 40 high, Vela's
// choice: M3's 56 holds a floating label, which Vela's field doesn't have.
C.TextField {
    id: field
    property bool password: false
    property string styleVariant: Theme.controlsVariant
    readonly property bool expressive: styleVariant === "expressive"
    implicitWidth: 240
    implicitHeight: expressive ? 40 : 32
    color: Theme.text
    placeholderTextColor: expressive ? Theme.surfaceVariantText : Theme.textTertiary
    // Expressive: the selection in primary (M3 publishes none; Vela's, as
    // Fluent's accent).
    selectionColor: expressive ? Theme.primary : Theme.accent
    selectedTextColor: expressive ? Theme.primaryText : "white"
    font.pixelSize: Theme.fontBody
    leftPadding: expressive ? 16 : 11
    rightPadding: expressive ? 16 : 11
    verticalAlignment: TextInput.AlignVCenter
    echoMode: password ? TextInput.Password : TextInput.Normal
    passwordCharacter: "●"
    inputMethodHints: password ? (Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText) : Qt.ImhNone
    background: Fill {
        readonly property real stateLayer: field.hovered && !field.activeFocus ? Theme.stateHover : 0
        radius: field.expressive ? 0 : Theme.radius
        topLeftRadius: field.expressive ? Theme.shapeExtraSmall : Theme.radius
        topRightRadius: field.expressive ? Theme.shapeExtraSmall : Theme.radius
        color: field.expressive ? Theme.mix(Theme.surfaceContainerHighest, Theme.surfaceText, stateLayer)
            : field.activeFocus ? (Theme.light ? "#ffffff" : Qt.rgba(0.12, 0.12, 0.12, 0.7)) : field.hovered ? Theme.controlHover : Theme.control
        border.width: field.expressive ? 0 : 1
        border.color: Theme.controlStroke
        Rectangle {
            objectName: "indicator"
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; leftMargin: field.expressive ? 0 : 1; rightMargin: field.expressive ? 0 : 1 }
            height: field.activeFocus ? 2 : 1
            radius: field.expressive ? 0 : 1
            // The focus changes it at once, like a focus ring.
            color: field.expressive ? (field.activeFocus ? Theme.primary : Theme.surfaceVariantText)
                : field.activeFocus ? Theme.accentFill : Theme.controlStrokeStrong
            opacity: field.expressive || field.activeFocus ? 1 : 0.6
        }
    }
}
