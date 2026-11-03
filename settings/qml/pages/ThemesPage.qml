// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 lowell
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Themes: a theme file (theme= in vela.conf) with the
// colors and corner radii of the shell and the apps (docs/configuration.md,
// "Theme files"). Your own go in ~/.config/vela/themes/<name>.conf.
Page {
    id: page
    readonly property var choices: [{ name: "", text: qsTr("Vela (default)") }].concat(Prefs.visualThemes())
    readonly property int chosenIndex: {
        for (let i = 0; i < choices.length; ++i) {
            if (choices[i].name === Prefs.visualTheme) return i
        }
        return 0 // not installed (any more): Vela's own
    }

    PersonalizationPage.DesktopPreview {}

    CardGroup {
        Card {
            icon: "preferences-desktop-theme"
            title: qsTr("Theme")
            description: Prefs.visualTheme !== "" && page.chosenIndex === 0
                ? qsTr("\"%1\" isn't installed: Vela's own look is used").arg(Prefs.visualTheme)
                : qsTr("Colors and corners of the shell and the apps. A theme can bring its own accent color")
            trailing: Choice {
                model: page.choices
                currentIndex: page.chosenIndex
                onChosen: index => Prefs.visualTheme = page.choices[index].name
            }
        }
        Card {
            icon: "edit-entry"
            title: qsTr("Your own themes")
            description: qsTr("A file in ~/.config/vela/themes/<name>.conf appears here. Copy an installed one from /usr/share/vela/themes/ to start")
        }
    }
}
