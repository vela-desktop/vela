// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Start: the Start menu's style (startmenu-style= in
// vela.conf), over the theme.
Page {
    CardGroup {
        title: qsTr("Style")
        StyleGallery { surface: "startmenu" }
    }
    CardGroup {
        Card {
            icon: "edit-entry"
            title: qsTr("Your own styles")
            description: qsTr("A file in ~/.config/vela/themes/startmenu/<name>.conf appears here")
        }
    }
}
