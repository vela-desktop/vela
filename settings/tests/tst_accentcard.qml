// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Test
import "../qml"

// Colors' accent color (AccentCard.qml) against the theme in use: its own
// accent for both modes or for one, Vela's and the apps' modes, and what the
// grid, the text and the swatch say about it.
Item {
    id: root
    width: 900
    height: 700

    property var navigated: []
    function navigate(name) {
        navigated = navigated.concat([name])
    }

    Column {
        width: parent.width
        AccentCard {
            id: card
        }
    }

    TestCase {
        name: "AccentCard"
        when: windowShown

        function grid() {
            return findChild(card, "accentGrid")
        }
        function swatch() {
            return findChild(card, "accentInUse")
        }
        function themesButton() {
            const found = []
            const walk = item => {
                for (let i = 0; i < item.children.length; ++i) {
                    const child = item.children[i]
                    if (child.text === "Themes" && child.clicked !== undefined) {
                        found.push(child)
                    }
                    walk(child)
                }
            }
            walk(card)
            return found[0]
        }
        // As a theme file would: its tokens, then themesRevision.
        function themeTokens(tokens) {
            Prefs.tokens = { vela: tokens }
            ++Prefs.themesRevision
        }

        function init() {
            Prefs.reset()
            root.navigated = []
            // A wallpaper with a color, unless a test says otherwise.
            card.wallpaperColor = "#3a6ea5"
        }

        function sourceChoice() {
            return findChild(card, "accentSource")
        }

        // The source's choice writes appearance/accentSource, and shows it.
        function test_the_source_choice() {
            const choice = sourceChoice()
            compare(choice.count, 3)
            compare(choice.currentIndex, 0)
            compare(choice.textAt(0), "From the theme")
            compare(choice.textAt(1), "From the wallpaper")
            compare(choice.textAt(2), "My color")
            compare(choice.Accessible.name, "Accent color source")
            choice.chosen(1)
            compare(Prefs.accentSource, "wallpaper")
            compare(choice.currentIndex, 1)
            choice.chosen(2)
            compare(Prefs.accentSource, "chosen")
            Prefs.accentSource = "theme"
            compare(choice.currentIndex, 0)
        }

        // The wallpaper's color next to the choice, or why there is none.
        function test_the_wallpaper_color_shows() {
            const color = findChild(card, "accentWallpaperColor")
            verify(color.visible)
            compare(color.color, Qt.color("#3a6ea5"))
            compare(color.Accessible.name, "Wallpaper color: Blue")
            verify(!findChild(card, "accentNoWallpaperColor").visible)
            card.wallpaperColor = ""
            verify(!color.visible)
            verify(findChild(card, "accentNoWallpaperColor").visible)
        }

        // The user asks for the wallpaper's: the palette shows nowhere and
        // the card says why; the swatch is the wallpaper's color.
        function test_the_wallpaper_chosen_disables_the_grid() {
            Prefs.accentSource = "wallpaper"
            verify(!grid().enabled)
            compare(grid().opacity, 0.45)
            compare(card.description, "Taken from your wallpaper: it changes with it. Choose My color to pick one below")
            compare(swatch().color, Qt.color("#3a6ea5"))
            verify(sourceChoice().enabled)
            verify(!themesButton().visible)
            // My color: the palette is the accent again.
            Prefs.accentSource = "chosen"
            verify(grid().enabled)
            compare(swatch().color, Prefs.accent)
            compare(card.description, "Buttons, selections, active items in the taskbar and menus")
        }

        // The theme asks for the wallpaper's (color-source), the user leaves
        // it to the theme.
        function test_a_theme_asking_for_the_wallpaper_says_so() {
            themeTokens({ "color-source": "wallpaper" })
            verify(!grid().enabled)
            compare(card.description,
                "Vela Official, the theme in use, takes it from your wallpaper: it changes with it. Choose My color to pick one below")
            compare(swatch().color, Qt.color("#3a6ea5"))
            // My color wins over the theme's wish.
            sourceChoice().chosen(2)
            verify(grid().enabled)
            compare(swatch().color, Prefs.accent)
        }

        // A wallpaper without a usable color: the one chosen here is used,
        // so the palette is enabled, and the card says why.
        function test_a_wallpaper_without_color_falls_back_to_the_chosen_one() {
            card.wallpaperColor = ""
            Prefs.accentSource = "wallpaper"
            verify(grid().enabled)
            compare(swatch().color, Prefs.accent)
            compare(card.description, "Your wallpaper has no color to take, so the one chosen below is used")
            Prefs.accentSource = "theme"
            themeTokens({ "color-source": "wallpaper" })
            verify(grid().enabled)
            compare(card.description,
                "Vela Official, the theme in use, takes it from your wallpaper, which has no color to take, so the one chosen below is used")
        }

        // The theme's accent in one mode and the wallpaper's in the other:
        // both said.
        function test_the_wallpaper_and_a_theme_accent_for_one_mode() {
            themeTokens({ "dark.accent": "#00b294", "color-source": "wallpaper" })
            Prefs.appTheme = "light"
            verify(!grid().enabled)
            compare(card.description,
                "Vela Official, the theme in use, takes it from your wallpaper: it changes with it. Choose My color to pick one below. "
                + "In dark mode Vela Official, the theme in use, brings its own")
        }

        // The theme's accent in every mode in use: the source changes
        // nothing, so its choice is disabled too, with the theme's reason.
        function test_a_theme_accent_disables_the_source_choice() {
            themeTokens({ accent: "#e81123", "color-source": "wallpaper" })
            Prefs.accentSource = "wallpaper"
            verify(!sourceChoice().enabled)
            verify(!grid().enabled)
            verify(card.description.startsWith("Vela Official, the theme in use, brings its own accent color"))
            compare(swatch().color, Qt.color("#e81123"))
        }

        function test_without_a_theme_accent_the_grid_is_the_accent() {
            verify(grid().enabled)
            compare(grid().opacity, 1)
            compare(swatch().color, Prefs.accent)
            verify(!themesButton().visible)
            compare(card.description, "Buttons, selections, active items in the taskbar and menus")
        }

        function test_a_theme_accent_for_both_modes_disables_the_grid() {
            themeTokens({ accent: "#e81123" })
            verify(!grid().enabled)
            compare(grid().opacity, 0.45)
            verify(card.description.startsWith("Vela Official, the theme in use, brings its own accent color"))
            // The swatch shows the accent in use, the theme's.
            compare(swatch().color, Qt.color("#e81123"))
            verify(themesButton().visible)
            // Laid out in its row before the click.
            waitForRendering(card)
            mouseClick(themesButton())
            compare(root.navigated, ["themes"])
            // In light mode too.
            Prefs.appTheme = "light"
            Prefs.shellTheme = "light"
            verify(!grid().enabled)
        }

        // The theme's accent for dark mode only, with Vela and the apps dark:
        // the one chosen here shows nowhere now. Before, the grid stayed
        // enabled and the swatch showed the chosen color.
        function test_an_accent_for_the_mode_in_use_everywhere_disables_the_grid() {
            themeTokens({ "dark.accent": "#00b294" })
            verify(!grid().enabled)
            compare(card.description,
                "In dark mode Vela Official, the theme in use, brings its own accent color. The one chosen here shows in light mode")
            compare(swatch().color, Qt.color("#00b294"))
            verify(themesButton().visible)
        }

        function test_an_accent_for_another_mode_leaves_the_grid_and_says_so() {
            themeTokens({ "dark.accent": "#00b294" })
            // The apps light: the one chosen here shows in them.
            Prefs.appTheme = "light"
            verify(grid().enabled)
            compare(swatch().color, Prefs.accent)
            compare(card.description,
                "Buttons, selections, active items in the taskbar and menus. In dark mode Vela Official, the theme in use, brings its own")
            verify(!themesButton().visible)
            // Vela light too: the theme's shows in neither.
            Prefs.shellTheme = "light"
            verify(grid().enabled)
            // A light-mode accent, with Vela and the apps dark.
            themeTokens({ "light.accent": "#ff8c00" })
            Prefs.appTheme = "dark"
            Prefs.shellTheme = "dark"
            verify(grid().enabled)
            verify(card.description.endsWith("In light mode Vela Official, the theme in use, brings its own"))
        }

        function swatches() {
            const grid = findChild(card, "accentSwatches")
            const list = []
            for (let i = 0; i < grid.children.length; ++i) {
                if (grid.children[i].modelData !== undefined) {
                    list.push(grid.children[i])
                }
            }
            return list
        }

        // One stop for Tab, on the chosen color; arrows move, Space chooses.
        function test_the_keyboard_moves_and_chooses() {
            const all = swatches()
            compare(all.length, 48)
            const stops = all.filter(s => s.focusPolicy === Qt.TabFocus)
            compare(stops.length, 1)
            compare(stops[0].modelData, "#0078d4") // Prefs.reset()'s accent
            stops[0].forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Right)
            verify(all[18].activeFocus)
            keyClick(Qt.Key_Down)
            const columns = findChild(card, "accentSwatches").columns
            verify(all[18 + columns].activeFocus)
            keyClick(Qt.Key_Up)
            keyClick(Qt.Key_Left)
            keyClick(Qt.Key_Left)
            verify(all[16].activeFocus)
            compare(Prefs.accent, Qt.color("#0078d4")) // moving chooses nothing
            keyClick(Qt.Key_Space)
            compare(Prefs.accent, Qt.color(all[16].modelData))
            // Tab then lands where the arrows left it.
            compare(all.filter(s => s.focusPolicy === Qt.TabFocus).length, 1)
            compare(all.filter(s => s.focusPolicy === Qt.TabFocus)[0], all[16])
        }

        // An accent from outside the palette: Tab still enters, on the first color.
        function test_tab_enters_without_a_palette_color() {
            Prefs.accent = "#123456"
            const stops = swatches().filter(s => s.focusPolicy === Qt.TabFocus)
            compare(stops.length, 1)
            compare(stops[0], swatches()[0])
        }

        // Renamed in its file: the text follows at once.
        function test_the_theme_name_follows_a_renamed_file() {
            themeTokens({ accent: "#e81123" })
            const themes = Prefs.themes.slice()
            themes[0] = Object.assign({}, themes[0], { text: "Vela" })
            Prefs.themes = themes
            ++Prefs.themesRevision
            verify(card.description.startsWith("Vela, the theme in use"))
        }

        function test_screen_readers_hear_names_and_places() {
            const all = swatches()
            compare(all[17].Accessible.role, Accessible.RadioButton)
            compare(all[17].Accessible.name, "Blue")
            compare(all[17].Accessible.description, "Color 18 of 48")
            verify(all[17].Accessible.checked)
            compare(all[0].Accessible.name, "Blue, light")
            compare(all[1].Accessible.name, "Yellow")
            compare(all[10].Accessible.name, "Red")
            compare(all[24].Accessible.name, "Purple")
            compare(all[41].Accessible.name, "Gray")
        }

        // What the swatches' accessible interfaces report (Qt's button
        // writes its own check state over the attached one once
        // accessibility is on): the chosen one checked, from the start, and
        // the choice moving with the keyboard and a click.
        function test_screen_readers_hear_the_chosen_color() {
            const all = swatches()
            for (const [i, chosen] of [[17, true], [16, false]]) {
                const state = AccessibleProbe.state(all[i])
                compare(state.role, Accessible.RadioButton)
                verify(state.checkable, `swatch ${i}`)
                compare(state.checked, chosen, `swatch ${i}`)
            }
            mouseClick(all[16])
            verify(AccessibleProbe.state(all[16]).checked)
            verify(!AccessibleProbe.state(all[17]).checked)
            // A click on the chosen one leaves it chosen, and heard so.
            mouseClick(all[16])
            verify(AccessibleProbe.state(all[16]).checked)
            AccessibleProbe.restart()
            verify(AccessibleProbe.state(all[16]).checked, "a screen reader started later")
            verify(AccessibleProbe.state(all[16]).checkable)
        }

        function test_choosing_a_color() {
            // The grid's third swatch is #ff8c00.
            const swatches = []
            const walk = item => {
                for (let i = 0; i < item.children.length; ++i) {
                    const child = item.children[i]
                    if (child.modelData === "#ff8c00") {
                        swatches.push(child)
                    }
                    walk(child)
                }
            }
            walk(grid())
            compare(swatches.length, 1)
            mouseClick(swatches[0])
            compare(Prefs.accent, Qt.color("#ff8c00"))
            compare(swatch().color, Qt.color("#ff8c00"))
        }
    }
}
