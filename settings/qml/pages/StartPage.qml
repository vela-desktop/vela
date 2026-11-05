// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Start: the Start menu's style (startmenu-style= in
// vela.conf), over the theme. The page has no other setting, so the gallery
// shows at once.
Page {
    CardGroup {
        title: qsTr("Style")
        StyleGallery {
            surface: "startmenu"
        }
    }
}
