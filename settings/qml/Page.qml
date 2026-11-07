// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic as C
import Vela.Controls

// A page's content: it scrolls, at most 1000 px wide as on Windows. Children
// go in a column. At its end, room for Undo (UndoBar, 56 tall and 24 above
// the window's bottom, over the page): scrolled to the end, the last card
// sits above it instead of under it.
Flickable {
    id: page
    default property alias content: column.data
    property alias spacing: column.spacing
    contentWidth: width
    contentHeight: column.height + 96
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    C.ScrollBar.vertical: C.ScrollBar {
        policy: page.contentHeight > page.height ? C.ScrollBar.AsNeeded : C.ScrollBar.AlwaysOff
    }

    Column {
        id: column
        x: 0
        width: Math.min(page.width - 24, 1000)
        spacing: 4
    }
}
