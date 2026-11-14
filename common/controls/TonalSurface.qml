// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// A Vela-owned tonal surface for expressive cards and layouts. Visual only:
// interactive children provide their own input, focus and accessibility.
// `level` is one of M3's surface containers: "lowest", "low", "container",
// "high", "highest"; `highEmphasis` is "high" with medium corners.
Rectangle {
    id: surface
    property bool highEmphasis: false
    property bool outlined: false
    property string level: highEmphasis ? "high" : "container"
    radius: highEmphasis ? Theme.shapeMedium : Theme.shapeLarge
    color: level === "lowest" ? Theme.surfaceContainerLowest
         : level === "low" ? Theme.surfaceContainerLow
         : level === "high" ? Theme.surfaceContainerHigh
         : level === "highest" ? Theme.surfaceContainerHighest
         : Theme.surfaceContainer
    border.width: outlined ? 1 : 0
    // M3: multi-element containers take outline variant, not outline.
    border.color: Theme.outlineVariant
}
