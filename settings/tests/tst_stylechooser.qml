// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// A surface's styles on its page: StyleChooser.qml, closed under a card that
// says the style in use (Taskbar), and the gallery in it (StyleGallery.qml),
// where a click applies a style at once and Undo goes back.
Item {
    id: root
    width: 1000
    height: 900

    property var offers: []
    // Main.qml's: what navigate() asks the page to show.
    property string reveal: ""
    function offerUndo(text, undo) {
        offers = offers.concat([{ text: text, undo: undo }])
    }

    Flickable {
        anchors.fill: parent
        contentHeight: column.height
        Column {
            id: column
            width: parent.width
            StyleChooser {
                id: chooser
                surface: "taskbar"
                title: "Taskbar"
            }
        }
    }

    // A page with the styles far down, as Taskbar has them.
    Component {
        id: longPage
        Flickable {
            width: 600
            height: 400
            contentHeight: content.height
            property alias chooser: farChooser
            Column {
                id: content
                width: parent.width
                Item { width: 1; height: 1500 }
                StyleChooser {
                    id: farChooser
                    surface: "notifications"
                    title: "Quick settings"
                }
                Item { width: 1; height: 300 }
            }
        }
    }

    Component {
        id: fresh
        StyleChooser {
            surface: "notifications"
        }
    }

    TestCase {
        name: "StyleChooser"
        when: windowShown

        function header() {
            return findChild(chooser, "styleHeader")
        }
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
            walk(chooser)
            return found
        }
        function tile(text) {
            return tiles().find(t => t.text === text)
        }

        function init() {
            Prefs.reset()
            root.offers = []
            chooser.expanded = false
        }

        function test_it_comes_closed() {
            const other = createTemporaryObject(fresh, column)
            verify(other)
            verify(!other.expanded)
            verify(!findChild(other, "styleBody").visible)
        }

        function test_closed_it_says_the_style_in_use() {
            verify(!findChild(chooser, "styleBody").visible)
            compare(tiles().length, 0) // not even made
            compare(header().description, "From the theme, as Vela Official draws it")
            Prefs.setSurfaceStyle("taskbar", "dock")
            compare(header().description, "Dock")
        }

        function test_a_click_or_the_keyboard_opens_and_closes_it() {
            mouseClick(header())
            verify(chooser.expanded)
            tryVerify(() => tiles().length === 3) // From the theme, Pill, Dock
            verify(findChild(chooser, "styleBody").visible)
            mouseClick(header())
            verify(!chooser.expanded)
            // Tab, then Space or Enter, as a card that opens a page.
            const button = header().children.find(c => c.Accessible.role === Accessible.Button)
            button.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            verify(chooser.expanded)
            keyClick(Qt.Key_Return)
            verify(!chooser.expanded)
        }

        function test_a_style_applies_at_once_and_undo_goes_back() {
            Prefs.setSurfaceStyle("taskbar", "pill")
            Prefs.calls = []
            chooser.expanded = true
            tryVerify(() => tiles().length === 3)
            // Laid out in their grid before the clicks.
            waitForRendering(chooser)
            verify(tile("Pill").selected)
            mouseClick(tile("Dock"))
            compare(Prefs.surfaceStyles.taskbar, "dock")
            verify(tile("Dock").selected)
            compare(root.offers.length, 1)
            compare(root.offers[0].text, "Dock is in use")
            root.offers[0].undo()
            compare(Prefs.surfaceStyles.taskbar, "pill")
            // From the theme: the style key empties.
            mouseClick(tile("From the theme"))
            compare(Prefs.surfaceStyles.taskbar, "")
            compare(root.offers[1].text, "The theme's style is in use")
            // The one in use again: nothing to apply, nothing to undo.
            mouseClick(tile("From the theme"))
            compare(root.offers.length, 2)
            compare(Prefs.calls.length, 3)
        }

        // The gallery is one stop for Tab, on the style in use; the arrows
        // move (one column for the taskbar's strips), Space applies.
        function test_one_tab_stop_and_the_arrows() {
            Prefs.setSurfaceStyle("taskbar", "pill")
            chooser.expanded = true
            tryVerify(() => tiles().length === 3)
            const stops = tiles().filter(t => t.focusPolicy === Qt.TabFocus)
            compare(stops.map(t => t.text), ["Pill"])
            stops[0].forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Up)
            verify(tile("From the theme").activeFocus)
            keyClick(Qt.Key_Down)
            keyClick(Qt.Key_Down)
            verify(tile("Dock").activeFocus)
            compare(Prefs.surfaceStyles.taskbar, "pill")
            keyClick(Qt.Key_Return)
            compare(Prefs.surfaceStyles.taskbar, "dock")
            compare(tiles().filter(t => t.focusPolicy === Qt.TabFocus).map(t => t.text), ["Dock"])
        }

        // vela.conf names a style that is gone (removed, or one of the names
        // before the renames): the theme's look is in use, and it says so.
        function test_a_style_that_isnt_installed_says_so() {
            Prefs.setSurfaceStyle("taskbar", "windows-10")
            compare(header().description, "\"windows-10\" isn't installed: the theme's look is in use")
            chooser.expanded = true
            tryVerify(() => tiles().length === 3)
            verify(findChild(chooser, "missingStyle").visible)
            verify(tile("From the theme").selected)
            verify(!tile("Pill").selected)
            // Choosing one puts things right.
            waitForRendering(chooser)
            mouseClick(tile("Pill"))
            verify(!findChild(chooser, "missingStyle").visible)
            verify(!tile("From the theme").selected)
        }

        // From the theme: the theme's pictures, or those of the theme it
        // builds on, and then the card says whose.
        function test_from_the_theme_names_the_theme_it_builds_on() {
            const picture = Qt.resolvedUrl("../../themes/screenshots/themes/vela/taskbar.png").toString()
            Prefs.visualTheme = "mine"
            Prefs.surfacePictures = { "mine taskbar": { images: [picture], from: "glass" } }
            ++Prefs.themesRevision
            chooser.expanded = true
            tryVerify(() => tiles().length === 3)
            compare(tile("From the theme").subtitle, "As Mine draws it, based on Glass")
            Prefs.surfacePictures = { "mine taskbar": { images: [picture], from: "mine" } }
            ++Prefs.themesRevision
            compare(tile("From the theme").subtitle, "As Mine draws it")
        }

        // A search for its style, or Themes' In use: open, in view, focused.
        function test_asked_for_it_opens_and_comes_into_view() {
            const page = createTemporaryObject(longPage, root)
            const far = page.chooser
            verify(!far.expanded)
            root.reveal = "taskbar" // another style's
            verify(!far.expanded)
            root.reveal = ""
            root.reveal = "notifications"
            verify(far.expanded)
            tryVerify(() => page.contentY > 1400, 2000, "not scrolled to it")
            const top = far.mapToItem(page, 0, 0).y
            verify(top >= 0 && top < page.height, "out of view at " + top)
            verify(findChild(far, "styleHeader").children.some(c => c.activeFocus))
            root.reveal = ""
        }

        function test_your_own_styles_are_named() {
            chooser.expanded = true
            tryVerify(() => tiles().length === 3)
            const texts = []
            const walk = item => {
                for (let i = 0; i < item.children.length; ++i) {
                    if (item.children[i].text !== undefined && typeof item.children[i].text === "string") {
                        texts.push(item.children[i].text)
                    }
                    walk(item.children[i])
                }
            }
            walk(chooser)
            verify(texts.indexOf("Your own styles: a file in ~/.config/vela/themes/taskbar/<name>.conf appears here") >= 0)
            // And their folder, opened.
            waitForRendering(chooser)
            Prefs.calls = []
            mouseClick(findChild(chooser, "openFolder"))
            compare(Prefs.calls, [["openThemesFolder", "taskbar"]])
        }
    }
}
