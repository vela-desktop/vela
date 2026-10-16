// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Keyboard focus, like Windows: a ring 2 units wide, 3 outside what it
// surrounds (its parent, with the parent's corners), shown at once. The
// control binds `visible` to its `visualFocus`: focus from the keyboard only.
Rectangle {
    anchors.fill: parent
    anchors.margins: -3
    radius: (parent.radius !== undefined ? parent.radius : 0) + 3
    color: "transparent"
    border.width: 2
    border.color: Theme.text
}
