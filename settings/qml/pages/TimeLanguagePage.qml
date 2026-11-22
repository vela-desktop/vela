// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

Page {
    CardGroup {
        LinkCard {
            icon: "preferences-system-time"
            title: qsTr("Date & time")
            description: qsTr("Time zone, automatic clock settings, time format")
            onClicked: root.navigate("datetime")
        }
        Card {
            icon: "preferences-desktop-locale"
            title: qsTr("Vela language")
            description: qsTr("The language of Vela's menus, windows and settings")
            // Language names stay in their own language, like on Windows.
            trailing: Choice {
                model: [qsTr("Same as the system"), "Italiano", "English", "Português"]
                currentIndex: Prefs.language === "it" ? 1 : Prefs.language === "en" ? 2 : Prefs.language === "pt" ? 3 : 0
                onChosen: index => Prefs.language = ["", "it", "en", "pt"][index]
            }
        }
    }
}
