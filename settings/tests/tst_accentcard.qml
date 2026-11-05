// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
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
            compare(all.filter(s => s.focusPolicy === Qt.TabFocus)[0], all[16])
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
