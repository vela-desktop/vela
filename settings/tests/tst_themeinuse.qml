// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Themes' summary (ThemeInUseCard.qml): the theme, each surface's style and
// where the accent comes from, each line leading to where it is chosen.
Item {
    id: root
    width: 900
    height: 500

    property var navigated: []
    function navigate(name, fromHistory, reveal) {
        navigated = navigated.concat([[name, reveal || ""]])
    }

    Column {
        width: parent.width
        ThemeInUseCard {
            id: card
        }
    }

    TestCase {
        name: "ThemeInUse"
        when: windowShown

        function line(name) {
            return findChild(card, name)
        }

        function init() {
            Prefs.reset()
            root.navigated = []
            card.wallpaperUsable = true
        }

        // The accent from the wallpaper: the user's wish or the theme's.
        function test_the_accent_from_the_wallpaper() {
            Prefs.accentSource = "wallpaper"
            compare(line("inUseAccent").value, "From the wallpaper")
            // A wallpaper without a usable color: the user's is used.
            card.wallpaperUsable = false
            compare(line("inUseAccent").value, "Yours")
            card.wallpaperUsable = true
            Prefs.accentSource = "theme"
            compare(line("inUseAccent").value, "Yours")
            Prefs.tokens = { vela: { "color-source": "wallpaper" } }
            ++Prefs.themesRevision
            compare(line("inUseAccent").value, "From the wallpaper")
            Prefs.tokens = { vela: { "color-source": "wallpaper", "dark.accent": "#00b294" } }
            ++Prefs.themesRevision
            compare(line("inUseAccent").value, "The theme's in dark mode, the wallpaper's in light mode")
            Prefs.tokens = { vela: { "dark.color-source": "wallpaper" } }
            ++Prefs.themesRevision
            compare(line("inUseAccent").value, "From the wallpaper in dark mode, yours in light mode")
            Prefs.accentSource = "chosen"
            compare(line("inUseAccent").value, "Yours")
        }

        function test_vela_own_look() {
            compare(card.title, "In use: Vela Official")
            compare(line("inUseTaskbar").value, "From the theme")
            compare(line("inUseStart").value, "From the theme")
            compare(line("inUseNotifications").value, "From the theme")
            compare(line("inUseAccent").value, "Yours")
            compare(line("inUseTaskbar").Accessible.name, "Taskbar: From the theme")
        }

        function test_styles_and_accent_over_a_theme() {
            Prefs.visualTheme = "glass"
            Prefs.setSurfaceStyle("taskbar", "dock")
            Prefs.setSurfaceStyle("startmenu", "gone")
            Prefs.tokens = { glass: { "dark.accent": "#00b294" } }
            ++Prefs.themesRevision
            compare(card.title, "In use: Glass")
            compare(line("inUseTaskbar").value, "Dock")
            compare(line("inUseStart").value, "\"gone\" isn't installed")
            compare(line("inUseAccent").value, "The theme's in dark mode, yours in light mode")
            Prefs.tokens = { glass: { accent: "#00b294" } }
            ++Prefs.themesRevision
            compare(line("inUseAccent").value, "The theme's")
        }

        function test_each_line_leads_where_it_is_chosen() {
            waitForItemPolished(card)
            mouseClick(line("inUseTaskbar"))
            mouseClick(line("inUseStart"))
            line("inUseNotifications").forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            line("inUseAccent").forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Return)
            compare(root.navigated, [["taskbar", "taskbar"], ["start", ""], ["taskbar", "notifications"], ["colors", ""]])
        }
    }
}
