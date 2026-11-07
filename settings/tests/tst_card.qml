// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Card.qml: a clickable card (LinkCard: Personalization's Background,
// Colors, Themes...) is a button for the keyboard and screen readers too; a
// card that only holds controls is neither.
Item {
    width: 700
    height: 400

    Column {
        width: parent.width
        spacing: 8
        LinkCard {
            id: link
            icon: "preferences-desktop-color"
            title: "Colors"
            description: "Accent color, light or dark mode"
        }
        Card {
            id: plain
            title: "Task view"
            trailing: Switch {}
        }
    }
    SignalSpy { id: clicks; target: link; signalName: "clicked" }

    TestCase {
        name: "Card"
        when: windowShown

        function button(card) {
            for (let i = 0; i < card.children.length; ++i) {
                if (card.children[i].focusPolicy !== undefined && card.children[i].Accessible.role === Accessible.Button) {
                    return card.children[i]
                }
            }
            return null
        }

        function init() {
            clicks.clear()
        }

        function test_a_clickable_card_works_with_the_keyboard() {
            const b = button(link)
            verify(b)
            compare(b.focusPolicy, Qt.TabFocus)
            b.forceActiveFocus(Qt.TabFocusReason)
            verify(b.visualFocus)
            keyClick(Qt.Key_Space)
            keyClick(Qt.Key_Return)
            keyClick(Qt.Key_Enter)
            compare(clicks.count, 3)
            mouseClick(link)
            compare(clicks.count, 4)
        }

        function test_screen_readers_hear_a_button_with_its_title() {
            const b = button(link)
            compare(b.Accessible.role, Accessible.Button)
            compare(b.Accessible.name, "Colors")
            compare(b.Accessible.description, "Accent color, light or dark mode")
            verify(!b.Accessible.ignored)
        }

        function test_a_card_with_controls_is_no_button() {
            const b = button(plain)
            verify(b)
            verify(!b.enabled)
            compare(b.focusPolicy, Qt.NoFocus)
            verify(b.Accessible.ignored)
        }
    }
}
