// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Taskbar's alignment (TaskbarAlignmentCard.qml): it decides where the apps
// go unless a fitted pill does (Style.taskbarLayout(), the shell's rule);
// then it is disabled, shows what the pill does and says who decides.
Item {
    width: 900
    height: 400

    Column {
        width: parent.width
        TaskbarAlignmentCard {
            id: card
        }
    }

    TestCase {
        name: "TaskbarAlignment"
        when: windowShown

        function choice() {
            return findChild(card, "alignment")
        }
        // The theme in use's tokens, its taskbar style's merged, as a file would.
        function layout(tokens, fromStyle) {
            Prefs.tokens = { vela: tokens }
            Prefs.layoutFromStyle = fromStyle === true
            ++Prefs.themesRevision
        }

        function init() {
            Prefs.reset()
        }

        function test_vela_own_bar_follows_the_alignment() {
            verify(choice().enabled)
            compare(choice().opacity, 1)
            compare(card.description, "")
            compare(choice().currentIndex, 0)
            Prefs.taskbarAlignment = "left"
            compare(choice().currentIndex, 1)
            // A floating bar that isn't fitted still follows it.
            layout({ "taskbar-margin": 12, "taskbar-radius": 8 })
            verify(choice().enabled)
            compare(choice().currentIndex, 1)
        }

        function test_choosing_left() {
            choice().chosen(1)
            compare(Prefs.taskbarAlignment, "left")
            choice().chosen(0)
            compare(Prefs.taskbarAlignment, "center")
        }

        function test_a_theme_pill_disables_it_and_shows_the_center() {
            Prefs.taskbarAlignment = "left"
            layout({ "taskbar-radius": 8, "taskbar-fit": 1 })
            verify(!choice().enabled)
            compare(choice().opacity, 0.45)
            // What the taskbar does, not what was chosen.
            compare(choice().currentIndex, 0)
            compare(card.description,
                "Vela Official keeps the apps centered in its pill: the alignment applies with another theme")
        }

        function test_a_style_pill_at_the_left_says_so() {
            Prefs.setSurfaceStyle("taskbar", "pill")
            layout({ "taskbar-radius": 8, "taskbar-fit": 1, "taskbar-edge": 2 }, true)
            verify(!choice().enabled)
            compare(choice().currentIndex, 1)
            compare(card.description, "Pill keeps the apps at the left: the alignment applies with another style")
        }

        // A style chosen that leaves the layout to the theme: the theme decides,
        // and another style wouldn't give the alignment back.
        function test_a_style_without_a_layout_leaves_it_to_the_theme() {
            Prefs.setSurfaceStyle("taskbar", "pill")
            layout({ "taskbar-radius": 8, "taskbar-fit": 1 }, false)
            compare(card.description,
                "Vela Official keeps the apps centered in its pill: the alignment applies with another theme")
        }

        function test_in_vela_mode() {
            // A pill in dark mode only: with Vela light, the alignment is back.
            layout({ "taskbar-radius": 8, "dark.taskbar-fit": 1, "light.taskbar-fit": 0 })
            verify(!choice().enabled)
            Prefs.shellTheme = "light"
            verify(choice().enabled)
        }
    }
}
