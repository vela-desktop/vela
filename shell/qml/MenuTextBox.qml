// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Windows 11's text box (TextBox) in the shell, with the Windows right-click
// menu (TextMenu).
TextBox {
    property alias mapToScreen: menu.mapToScreen
    persistentSelection: true // the menu takes the keyboard: the selection must stay
    Keys.onPressed: event => menu.key(event)

    TextMenu { id: menu }
}
