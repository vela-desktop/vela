// SPDX-FileCopyrightText: 2026 Lowell137
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// WindowGlass's border (quicksettings-borderless=1): 1 px of #50808080 along
// one edge of a rounded shape and its corners, the top (a panel) or the
// bottom (a tile). Put it inside the shape, which it fills in width.
Item {
    id: edge
    property bool atBottom: false
    property real cornerRadius
    anchors { left: parent.left; right: parent.right }
    y: atBottom ? parent.height - height : 0
    height: Math.max(1, cornerRadius)
    clip: true
    enabled: false // clicks go through
    Rectangle {
        width: parent.width
        height: edge.parent ? edge.parent.height : 0
        y: edge.atBottom ? parent.height - height : 0
        radius: edge.cornerRadius
        color: "transparent"
        border.width: 1
        border.color: "#50808080"
    }
}
