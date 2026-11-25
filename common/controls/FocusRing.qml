// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Keyboard focus, like Windows: a ring 2 units wide, 3 outside what it
// surrounds (its parent, with the parent's corners), shown at once. The
// control binds `visible` to its `visualFocus`: focus from the keyboard only.
// Expressive (the control's `expressive`, else the theme's): the same ring in
// secondary, Vela's choice (the M3 references ask only for a ring-like
// indicator): a tonal accent that reads at 3:1 on every surface container,
// apart from the primary fills it surrounds.
Rectangle {
    property bool expressive: Theme.expressive
    anchors.fill: parent
    anchors.margins: -3
    radius: (parent.radius !== undefined ? parent.radius : 0) + 3
    color: "transparent"
    border.width: 2
    border.color: expressive ? Theme.secondary : Theme.text
}
