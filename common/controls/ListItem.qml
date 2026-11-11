// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T

// A row of a list (a network, a sound output, an option): Qt's item delegate,
// a click, Space or Enter activate it. Subtle at rest; `checked`, the current
// one of a subtle list (a navigation entry, a tool), rests on the hover color;
// `highlighted`, the chosen one, takes the control's colors; pressed over
// hovered over rest, faded (Fill). Its content is its children, placed by the
// caller.
//
// Expressive (M3's expressive list): 56 high and 16 on each side unless the
// caller sizes it; hovered and pressed are state layers of on surface in
// rounded corners (medium, 12); `checked` and `highlighted` are the selected
// item, a full pill of secondary container with its state layers and the
// label in Medium (Vela's: M3's navigation drawer's active indicator, the
// selection the plan gives every list). The corners morph on the fast
// spatial curve.
T.ItemDelegate {
    id: control
    property string styleVariant: Theme.controlsVariant
    readonly property bool expressive: styleVariant === "expressive"
    readonly property bool selected: checked || highlighted
    readonly property real stateLayer: down ? Theme.statePressed : hovered ? Theme.stateHover : 0

    implicitHeight: expressive ? 56 : 0 // Fluent: the caller's, as Qt's template
    leftPadding: expressive ? 16 : padding
    rightPadding: expressive ? 16 : padding
    font.weight: expressive && selected ? Font.Medium : Font.Normal
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    background: Fill {
        radius: !control.expressive ? Theme.radius
            : control.selected ? height / 2
            : Math.min(Theme.shapeMedium, height / 2)
        color: control.expressive
            ? (control.selected
                ? Theme.mix(Theme.secondaryContainer, Theme.secondaryContainerText, control.stateLayer)
                : Qt.rgba(Theme.surfaceText.r, Theme.surfaceText.g, Theme.surfaceText.b, control.stateLayer))
            : control.highlighted ? (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control)
            : (control.down ? Theme.subtlePressed : control.hovered || control.checked ? Theme.subtleHover : Theme.subtleRest)
        Behavior on radius {
            enabled: control.expressive && Theme.motion > 0
            Spatial { speed: "fast"; rest: Theme.fast }
        }
        FocusRing { visible: control.visualFocus; expressive: control.expressive }
    }
}
