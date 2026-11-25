// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// The theme's miniature (DesktopPreview.qml): a theme with the expressive
// controls draws with the tonal roles of the palette it would start from,
// so a theme without a picture previews as it looks; the default look keeps
// its Fluent colors.
Item {
    id: root
    width: 400
    height: 300

    DesktopPreview {
        id: preview
        framed: false
    }

    TestCase {
        name: "DesktopPreview"
        when: windowShown

        function windowColor() {
            return findChild(preview, "previewWindow").color
        }
        function barColor() {
            return findChild(preview, "previewBar").color
        }

        function init() {
            Prefs.reset()
            preview.themeTokens = {}
        }

        function test_the_default_look_keeps_its_colors() {
            verify(Qt.colorEqual(windowColor(), Qt.rgba(0.13, 0.13, 0.13, 0.96)))
            verify(Qt.colorEqual(barColor(), Qt.rgba(0.11, 0.11, 0.13, 0.85)))
            compare(preview.appsTonal, null)
        }

        function test_an_expressive_theme_previews_its_tonal_roles() {
            preview.themeTokens = { "controls-variant": "expressive", accent: "#7a5af8" }
            const dark = Style.tonalOf("#7a5af8", false)
            compare(windowColor(), dark.surface)
            compare(barColor(), dark.surfaceContainer)
            // Light mode: the light roles.
            Prefs.appTheme = "light"
            Prefs.shellTheme = "light"
            const light = Style.tonalOf("#7a5af8", true)
            compare(windowColor(), light.surface)
            compare(barColor(), light.surfaceContainer)
        }

        // Without an accent of its own, the user's: the one chosen, or the
        // wallpaper's when asked.
        function test_an_expressive_theme_without_accent_takes_the_user_one() {
            preview.themeTokens = { "controls-variant": "expressive" }
            compare(windowColor(), Style.tonalOf(Prefs.accent, false).surface)
            if (Style.wallpaperColor.valid) {
                preview.themeTokens = { "controls-variant": "expressive", "color-source": "wallpaper" }
                compare(windowColor(), Style.tonalOf(Style.wallpaperColor, false).surface)
            }
        }

        // The theme's own colors still win.
        function test_the_theme_colors_win() {
            preview.themeTokens = { "controls-variant": "expressive", accent: "#7a5af8", background: "#102030" }
            compare(windowColor(), Qt.color("#102030"))
        }
    }
}
