// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Start: the Start button's icon and what the Start menu
// shows, then its style (startmenu-style= in vela.conf, over the theme),
// whose gallery shows at once.
Page {
    CardGroup {
        title: qsTr("Start")
        StartIconCard {}
        Card {
            icon: "pin"
            title: qsTr("Show pinned apps")
            description: qsTr("Above all apps. Turned off, Start opens on all apps and keeps your pins")
            trailing: Switch {
                objectName: "startPinned"
                checked: Prefs.startPinned
                onToggled: Prefs.startPinned = checked
            }
        }
    }

    CardGroup {
        title: qsTr("Style")
        StyleGallery {
            surface: "startmenu"
        }
    }
}
