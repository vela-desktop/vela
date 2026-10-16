// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A bare shell text field, for a frame of its own, with the Windows
// right-click menu (TextMenu).
TextInput {
    id: input
    property alias mapToScreen: menu.mapToScreen

    color: Theme.text
    selectionColor: Theme.accent
    font.pixelSize: Theme.fontBody
    selectByMouse: true
    persistentSelection: true // the menu takes the keyboard: the selection must stay
    clip: true
    Keys.onPressed: event => menu.key(event)

    TextMenu { id: menu }
}
