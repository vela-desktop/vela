// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Windows right-click menu (Undo, Cut, Copy, Paste, Delete, Select all)
// of the text field it is in: a right click on it, or the Menu key and
// Shift+F10 the field passes on with `Keys.onPressed: event => menu.key(event)`.
MouseArea {
    id: menu
    readonly property Item input: parent
    // From field to output coordinates: every shell window knows where it is.
    property var mapToScreen: (x, y) => input.mapToItem(null, x, y)

    anchors.fill: parent
    acceptedButtons: Qt.RightButton
    cursorShape: Qt.IBeamCursor

    function open(x, y, keyboard) {
        const p = mapToScreen(x, y)
        Menus.open(Menus.textEntries(input), p.x, p.y, { keyboard: keyboard })
    }
    function key(event) {
        if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
            const r = input.cursorRectangle
            open(r.x, r.y + r.height, true)
            event.accepted = true
        }
    }

    onPressed: mouse => {
        input.forceActiveFocus()
        open(mouse.x, mouse.y, false)
    }
}
