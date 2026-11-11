// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// A Vela-owned tonal surface for expressive cards and layouts. Visual only:
// interactive children provide their own input, focus and accessibility.
Rectangle {
    id: surface
    property bool highEmphasis: false
    property bool outlined: false
    radius: highEmphasis ? Theme.shapeMedium : Theme.shapeLarge
    color: highEmphasis ? Theme.surfaceContainerHigh : Theme.surfaceContainer
    border.width: outlined ? 1 : 0
    border.color: Theme.outline
}
