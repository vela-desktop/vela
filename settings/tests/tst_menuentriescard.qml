// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// One menu in Personalization > Context menu (MenuEntriesCard.qml): the card
// counts the hidden entries, opens to a switch per entry, and a switch hides
// or shows its entry's keys (a group's all at once).
Item {
    id: root
    width: 700
    height: 700

    Column {
        width: parent.width
        MenuEntriesCard {
            id: card
            title: "Taskbar apps"
            about: "A right click"
            entries: [
                { keys: ["taskbar.pinned"], text: "Pinned files" },
                { keys: ["taskbar.recent", "taskbar.recentHeader"], text: "Recent files" },
                { keys: ["taskbar.close"], text: "Close window", icon: "window-close" }
            ]
        }
    }

    TestCase {
        name: "MenuEntriesCard"
        when: windowShown

        function init() {
            Prefs.reset()
            card.expanded = false
        }

        function toggle(key) {
            return findChild(card, "entry:" + key)
        }

        function test_the_card_counts_what_is_hidden() {
            const header = findChild(card, "menuHeader")
            compare(header.description, "A right click. All 3 entries shown")
            Prefs.hiddenMenuEntries = ["taskbar.close", "other.menu"]
            compare(header.description, "A right click. 1 hidden")
            Prefs.hiddenMenuEntries = ["taskbar.recentHeader"]
            compare(card.hiddenCount, 1, "one key of a group hides the group")
        }

        function test_switches_show_when_opened() {
            verify(!toggle("taskbar.pinned"), "closed: no switches made")
            mouseClick(findChild(card, "menuHeader"))
            verify(card.expanded)
            tryVerify(() => toggle("taskbar.pinned") && toggle("taskbar.pinned").visible)
            verify(toggle("taskbar.close").checked)
            compare(toggle("taskbar.close").Accessible.name, "Close window")
        }

        function test_a_switch_hides_and_shows_its_keys() {
            card.expanded = true
            tryVerify(() => toggle("taskbar.recent"))
            waitForRendering(root)
            const recent = toggle("taskbar.recent")
            mouseClick(recent)
            compare(Prefs.calls, [["setMenuEntriesHidden", ["taskbar.recent", "taskbar.recentHeader"], true]])
            verify(!recent.checked)
            mouseClick(recent)
            compare(Prefs.hiddenMenuEntries, [])
            verify(recent.checked)
        }

        function test_a_refused_write_puts_the_switch_back() {
            card.expanded = true
            tryVerify(() => toggle("taskbar.close"))
            waitForRendering(root)
            Prefs.refuseMenuWrites = true
            mouseClick(toggle("taskbar.close"))
            compare(Prefs.calls.length, 1)
            verify(toggle("taskbar.close").checked, "still shown, so still on")
            Prefs.refuseMenuWrites = false
            mouseClick(toggle("taskbar.close"))
            compare(Prefs.calls[1], ["setMenuEntriesHidden", ["taskbar.close"], true], "the next click hides it again")
            verify(!toggle("taskbar.close").checked)
        }

        function test_a_switch_follows_a_change_made_elsewhere() {
            card.expanded = true
            tryVerify(() => toggle("taskbar.pinned"))
            Prefs.hiddenMenuEntries = ["taskbar.pinned"]
            verify(!toggle("taskbar.pinned").checked)
            Prefs.showAllMenuEntries()
            verify(toggle("taskbar.pinned").checked)
        }
    }
}
