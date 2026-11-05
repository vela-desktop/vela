// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Themes' way to your own themes: where they go and how to start one, and
// the folder itself, opened in the file manager (made if it isn't there yet).
Card {
    icon: "edit-entry"
    title: qsTr("Your own themes")
    description: qsTr("A file in ~/.config/vela/themes/<name>.conf appears here. Start it with inherits=vela, then set only the colors and corners you want to change")
    trailing: Button {
        objectName: "openFolder"
        text: qsTr("Open folder")
        onClicked: Prefs.openThemesFolder("")
    }
}
