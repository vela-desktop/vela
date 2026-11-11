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

    // Expressive: M3's small button (40 high, 16 on each side; an icon-only
    // one is the 40 small icon button). Its 20 px icon stays Vela's 16, the
    // icon theme's grid; disabled stays Vela's 0.45 opacity in both looks.
    implicitWidth: Math.max(subtle && text === "" ? (expressive ? 40 : 32) : (expressive ? 88 : 96), implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: expressive ? 40 : 32
    leftPadding: expressive ? (text === "" ? 8 : 16) : 12
    rightPadding: expressive ? (text === "" ? 8 : 16) : 12
    spacing: 8
    focusPolicy: Qt.TabFocus
    hoverEnabled: true // not dependent on platform style hints (also works offscreen)
    opacity: enabled ? 1 : 0.45
    icon.width: 16
    icon.height: 16
    font.pixelSize: Theme.fontBody
    // M3's label large is Medium.
    font.weight: expressive ? Font.Medium : Font.Normal

    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    // Expressive: Material's state layer (Theme.stateHover, statePressed)
    // of the content's color over the fill, so the states read in both
    // modes; a subtle button is M3's text button, with no fill at rest.
    readonly property real stateLayer: down ? Theme.statePressed
        : hovered || (subtle && checked) ? Theme.stateHover : 0
    // Expressive shapes (M3's buttons): round at rest, the pressed shape
    // (small, 8) while pressed, square (medium, 12) while checked, as a
    // selected toggle button. Fluent: Theme.radius.
    readonly property real shape: !expressive ? Theme.radius
        : down ? Theme.shapeSmall
        : checked ? Theme.shapeMedium
        : height / 2

    background: Fill {
        radius: Math.min(control.shape, height / 2)
        color: control.expressive
            ? (control.subtle
                ? Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, control.stateLayer)
                : control.accent
                    ? Theme.mix(Theme.primary, Theme.primaryText, control.stateLayer)
                    : Theme.mix(Theme.secondaryContainer, Theme.secondaryContainerText, control.stateLayer))
            : (control.subtle ? (control.down ? Theme.subtlePressed : control.hovered || control.checked ? Theme.subtleHover : Theme.subtleRest)
                : control.accent ? (control.hovered ? Theme.accentFillHover : Theme.accentFill)
                : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control))
        border.width: control.expressive ? 0 : control.subtle ? 0 : 1
        border.color: control.accent ? Qt.rgba(0, 0, 0, 0.1) : Theme.controlStroke
        // The corners morph on M3's fast spatial curve (Spatial); the
        // default look's radius doesn't change, so nothing moves there.
        Behavior on radius {
            enabled: control.expressive && Theme.motion > 0
            Spatial { speed: "fast"; rest: Theme.fast }
        }
        // At once, like a disabled look: both must read immediately.
        FocusRing { visible: control.visualFocus; expressive: control.expressive }
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
