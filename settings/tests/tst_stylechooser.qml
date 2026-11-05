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
        }
    }
}
