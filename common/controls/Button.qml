// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T

// Windows 11's button: standard (gray), accent, or subtle (no background,
// like the cards' "..." buttons; `checked`, what it opens is open, rests on
// the hover color), with text, an icon (`icon.name`, from Theme.icons) or
// both. Qt's button (input, Space and focus, accessibility),
// Vela's look: its color fades between states (Fill). Tab gives it the focus
// and a ring, a click doesn't, so the keyboard stays where it was; Enter
// activates it too, like Space.
T.Button {
    id: control
    property bool accent: false
    property bool subtle: false
    property alias usable: control.enabled
    // The theme normally selects this; an instance may override it for previews.
    property string styleVariant: Theme.controlsVariant
    readonly property bool expressive: styleVariant === "expressive"

    implicitWidth: Math.max(subtle && text === "" ? (expressive ? 40 : 32) : (expressive ? 88 : 96), implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: expressive ? 40 : 32
    leftPadding: expressive ? 20 : 12
    rightPadding: expressive ? 20 : 12
    spacing: 8
    focusPolicy: Qt.TabFocus
    hoverEnabled: true // not dependent on platform style hints (also works offscreen)
    opacity: enabled ? 1 : 0.45
    icon.width: 16
    icon.height: 16
    font.pixelSize: Theme.fontBody

    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    background: Fill {
        radius: control.expressive ? height / 2 : Theme.radius
        // Expressive states are layers of the content color over the fill
        // (8% hovered, 12% pressed), so they read in both modes.
        readonly property real stateLayer: control.down ? 0.12 : control.hovered || (control.subtle && control.checked) ? 0.08 : 0
        color: control.expressive
            ? (control.subtle
                ? Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, stateLayer)
                : control.accent
                    ? Theme.mix(Theme.primary, Theme.primaryText, stateLayer)
                    : Theme.mix(Theme.secondaryContainer, Theme.secondaryContainerText, stateLayer))
            : (control.subtle ? (control.down ? Theme.subtlePressed : control.hovered || control.checked ? Theme.subtleHover : Theme.subtleRest)
                : control.accent ? (control.hovered ? Theme.accentFillHover : Theme.accentFill)
                : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control))
        border.width: control.expressive ? 0 : control.subtle ? 0 : 1
        border.color: control.accent ? Qt.rgba(0, 0, 0, 0.1) : Theme.controlStroke
        // At once, like a disabled look: both must read immediately.
        FocusRing { visible: control.visualFocus }
    }

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            anchors.centerIn: parent
            spacing: control.spacing
            Icon {
                visible: name !== ""
                anchors.verticalCenter: parent.verticalCenter
                name: control.icon.name
                size: control.icon.width
                onAccent: control.accent
            }
            Text {
                objectName: "label"
                visible: control.text !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                color: control.expressive ? (control.accent ? Theme.primaryText : control.subtle ? Theme.primary : Theme.secondaryContainerText)
                                          : (control.accent ? Theme.accentText : Theme.text)
                font: control.font
            }
        }
    }
}
