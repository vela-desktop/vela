// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Colors: the mode (light, dark, or custom: one for Vela and
// one for apps) and the accent color, with the Windows 11 palette.
Page {
    id: page
    property bool custom: false // "Custom" chosen even with the two modes equal

    DesktopPreview {}

    CardGroup {
        Card {
            icon: "preferences-desktop-theme"
            title: qsTr("Choose your mode")
            description: qsTr("Change the colors of Vela and your apps (light or dark)")
            trailing: Choice {
                model: [qsTr("Light"), qsTr("Dark"), qsTr("Custom")]
                currentIndex: page.custom || Prefs.shellTheme !== Prefs.appTheme ? 2 : Prefs.appTheme === "light" ? 0 : 1
                onChosen: index => {
                    page.custom = index === 2
                    if (index < 2) {
                        const mode = index === 0 ? "light" : "dark"
                        Prefs.shellTheme = mode
                        Prefs.appTheme = mode
                    }
                }
            }
        }
        Card {
            visible: page.custom || Prefs.shellTheme !== Prefs.appTheme
            icon: "preferences-system-windows"
            title: qsTr("Choose your default Vela mode")
            description: qsTr("Taskbar, Start menu, menus and panels")
            trailing: Choice {
                model: [qsTr("Light"), qsTr("Dark")]
                currentIndex: Prefs.shellTheme === "light" ? 0 : 1
                onChosen: index => Prefs.shellTheme = index === 0 ? "light" : "dark"
            }
        }
        Card {
            visible: page.custom || Prefs.shellTheme !== Prefs.appTheme
            icon: "preferences-desktop-default-applications"
            title: qsTr("Choose your default app mode")
            description: qsTr("App windows, File Explorer and Settings")
            trailing: Choice {
                model: [qsTr("Light"), qsTr("Dark")]
                currentIndex: Prefs.appTheme === "light" ? 0 : 1
                onChosen: index => Prefs.appTheme = index === 0 ? "light" : "dark"
            }
        }
        // The accent color, and what the theme in use does to it.
        AccentCard {}
    }
}
