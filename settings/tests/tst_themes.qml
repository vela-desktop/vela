// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Personalization's themes: the gallery (ThemeGallery.qml), where a click or
// the keyboard applies a theme at once and Undo goes back, and Themes' Reset
// (ResetThemesCard.qml), whose Undo puts the theme and every style back.
Item {
    id: root
    width: 1000
    height: 800

    property var offers: []
    function offerUndo(text, undo) {
        offers = offers.concat([{ text: text, undo: undo }])
    }

    Column {
        width: parent.width
        spacing: 8
        ThemeGallery {
            id: gallery
        }
        OwnThemesCard {
            id: own
        }
        ResetThemesCard {
            id: reset
        }
    }

    TestCase {
        name: "Themes"
        when: windowShown

        function tiles() {
            const found = []
            const walk = item => {
                for (let i = 0; i < item.children.length; ++i) {
                    const child = item.children[i]
                    if (child.Accessible.role === Accessible.RadioButton && child.pictureHeight !== undefined) {
                        found.push(child)
                    }
                    walk(child)
                }
            }
            walk(gallery)
            return found
        }
        function tile(text) {
            return tiles().find(t => t.text === text)
        }
        function visibleOf(item, type) {
            const walk = node => {
                for (let i = 0; i < node.children.length; ++i) {
                    const child = node.children[i]
                    if (String(child).startsWith(type) && child.visible) {
                        return true
                    }
                    if (walk(child)) {
                        return true
                    }
                }
                return false
            }
            return walk(item)
        }

        function init() {
            Prefs.reset()
            root.offers = []
            tryVerify(() => tiles().length === 3)
            waitForRendering(gallery)
        }

        function test_a_theme_applies_at_once_and_undo_goes_back() {
            verify(tile("Vela Official").selected)
            mouseClick(tile("Glass"))
            compare(Prefs.visualTheme, "glass")
            verify(tile("Glass").selected)
            verify(!tile("Vela Official").selected)
            compare(root.offers.length, 1)
            compare(root.offers[0].text, "Glass is in use")
            root.offers[0].undo()
            compare(Prefs.visualTheme, "vela")
            // The theme in use again: nothing to apply, nothing to undo.
            waitForRendering(gallery)
            mouseClick(tile("Vela Official"))
            compare(root.offers.length, 1)
        }

        function test_the_keyboard_applies_one_too() {
            const mine = tile("Mine")
            mine.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            compare(Prefs.visualTheme, "mine")
            compare(root.offers[0].text, "Mine is in use")
            compare(tile("Mine").Accessible.checked, true)
        }

        // One stop for Tab, on the theme in use; the arrows move, Space applies.
        function test_one_tab_stop_and_the_arrows() {
            const stops = tiles().filter(t => t.focusPolicy === Qt.TabFocus)
            compare(stops.length, 1)
            compare(stops[0].text, "Vela Official")
            stops[0].forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Right)
            verify(tile("Glass").activeFocus)
            keyClick(Qt.Key_Right)
            verify(tile("Mine").activeFocus)
            keyClick(Qt.Key_Right) // the last: stays
            verify(tile("Mine").activeFocus)
            keyClick(Qt.Key_Left)
            compare(Prefs.visualTheme, "vela") // moving applies nothing
            keyClick(Qt.Key_Space)
            compare(Prefs.visualTheme, "glass")
            compare(tiles().filter(t => t.focusPolicy === Qt.TabFocus).map(t => t.text), ["Glass"])
        }

        // An installed theme shows its picture; one without (your own), the
        // desktop in miniature in its colors.
        function test_pictures_or_the_miniature() {
            const picture = Qt.resolvedUrl("../../themes/screenshots/themes/vela.png").toString()
            const themes = Prefs.themes.slice()
            themes[1] = Object.assign({}, themes[1], { images: [picture] })
            Prefs.themes = themes
            ++Prefs.themesRevision
            tryVerify(() => tiles().length === 3)
            verify(visibleOf(tile("Glass"), "QQuickImage"))
            verify(!visibleOf(tile("Glass"), "DesktopPreview"))
            verify(visibleOf(tile("Mine"), "DesktopPreview"))
        }

        function test_your_own_themes_folder_opens() {
            waitForItemPolished(own)
            mouseClick(findChild(own, "openFolder"))
            compare(Prefs.calls, [["openThemesFolder", ""]])
        }

        function test_reset_and_undo_put_everything_back() {
            const button = findChild(reset, "reset")
            verify(!button.enabled, "Reset with nothing to reset")
            Prefs.visualTheme = "glass"
            Prefs.setSurfaceStyle("taskbar", "pill")
            Prefs.setSurfaceStyle("startmenu", "square")
            Prefs.calls = []
            verify(button.enabled)
            waitForRendering(reset)
            mouseClick(button)
            compare(Prefs.visualTheme, "vela")
            compare(Prefs.surfaceStyles.taskbar, "")
            compare(Prefs.surfaceStyles.startmenu, "")
            compare(root.offers[0].text, "Vela Official is in use, with no styles")
            root.offers[0].undo()
            compare(Prefs.visualTheme, "glass")
            compare(Prefs.surfaceStyles.taskbar, "pill")
            compare(Prefs.surfaceStyles.startmenu, "square")
            compare(Prefs.surfaceStyles.notifications, "")
            // One write for Reset, one for its Undo.
            compare(Prefs.calls.map(c => c[0]), ["resetThemes", "setThemeAndStyles"])
        }
    }
}
