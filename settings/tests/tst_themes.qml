// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Test
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

        // What the tiles' accessible interfaces report: the theme in use
        // checked from the start, the others not; a click on the one in use
        // leaves it checked.
        function test_screen_readers_hear_the_theme_in_use() {
            const official = AccessibleProbe.state(tile("Vela Official"))
            compare(official.role, Accessible.RadioButton)
            verify(official.checkable)
            verify(official.checked)
            verify(!AccessibleProbe.state(tile("Mine")).checked)
            mouseClick(tile("Mine"))
            verify(AccessibleProbe.state(tile("Mine")).checked)
            verify(!AccessibleProbe.state(tile("Vela Official")).checked)
            mouseClick(tile("Mine"))
            verify(AccessibleProbe.state(tile("Mine")).checked)
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
            // Undone while Glass has the focus: once the focus leaves, Tab's
            // stop is the theme in use again, and only it (Qt Quick ignores a
            // focus policy taken away from the item with the focus).
            root.offers[0].undo()
            findChild(reset, "reset").forceActiveFocus()
            compare(tiles().filter(t => t.focusPolicy === Qt.TabFocus).map(t => t.text), ["Vela Official"])
        }

        // A theme in vela.conf that isn't installed: Tab still enters, on the first.
        function test_tab_enters_without_a_theme_in_use() {
            Prefs.visualTheme = "gone"
            compare(tiles().filter(t => t.focusPolicy === Qt.TabFocus).map(t => t.text), ["Vela Official"])
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
            const image = findChild(tile("Glass"), "picture")
            tryVerify(() => image.status === Image.Ready)
            verify(image.visible)
            verify(!visibleOf(tile("Glass"), "DesktopPreview"))
            verify(visibleOf(tile("Mine"), "DesktopPreview"))
        }

        // A picture that can't be read: the miniature, not an empty card.
        function test_a_broken_picture_shows_the_miniature() {
            const themes = Prefs.themes.slice()
            themes[1] = Object.assign({}, themes[1], { images: [Qt.resolvedUrl("tst_themes.qml").toString()] })
            Prefs.themes = themes
            ++Prefs.themesRevision
            tryVerify(() => tiles().length === 3)
            const image = findChild(tile("Glass"), "picture")
            tryVerify(() => image.status === Image.Error)
            verify(!image.visible)
            verify(visibleOf(tile("Glass"), "DesktopPreview"))
        }

        // A file the reader turns down: shown, faded, saying why, and not applied.
        function test_a_theme_vela_cant_use_isnt_applied() {
            Prefs.rejected = ["glass"]
            ++Prefs.themesRevision
            tryVerify(() => tiles().length === 3)
            waitForItemPolished(gallery)
            const glass = tile("Glass")
            verify(!glass.enabled)
            compare(glass.opacity, 0.45)
            compare(glass.subtitle, "Vela can't use this file: too large, or for another version")
            mouseClick(glass)
            compare(Prefs.visualTheme, "vela")
            compare(root.offers.length, 0)
        }

        // Renamed in its file: the names follow at once.
        function test_names_follow_a_renamed_file() {
            const themes = Prefs.themes.slice()
            themes[0] = Object.assign({}, themes[0], { text: "Vela" })
            Prefs.themes = themes
            ++Prefs.themesRevision
            compare(reset.velaName, "Vela")
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
