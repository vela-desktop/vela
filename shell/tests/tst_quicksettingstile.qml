// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Test
import "../qml"
import Vela.Shell.Test as ShellTest

// What a screen reader hears from quick settings' tiles, in both looks: the
// default tile (QuickSettingsTile.qml) and the expressive one (QsTile.qml).
// Read from the items' accessible interfaces (AccessibleProbe), not from the
// attached Accessible properties: Qt's button writes its own checkable and
// checked over those once accessibility is on, which once left every tile
// "not checkable, not checked" whatever it showed.
Item {
    id: root
    width: 600
    height: 300

    // The owner of a tile's state: it may refuse a change (a Wi-Fi switch
    // that NetworkManager refuses).
    property bool nightLight: true
    property bool wifi: false
    property bool refuse: false

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    QuickSettingsTile {
        id: plain
        x: 20
        y: 20
        style: ShellTest.Styles.quickSettings
        icon: "redshift-status-on"
        label: "Night light"
        checked: root.nightLight
        onToggled: if (!root.refuse) root.nightLight = !root.nightLight
    }
    QuickSettingsTile {
        id: split
        x: 140
        y: 20
        style: ShellTest.Styles.quickSettings
        icon: "network-wireless"
        label: "Wi-Fi"
        split: true
        checked: root.wifi
        onToggled: if (!root.refuse) root.wifi = !root.wifi
    }
    QuickSettingsTile {
        id: menu
        x: 260
        y: 20
        style: ShellTest.Styles.quickSettings
        icon: "preferences-desktop-accessibility"
        label: "Accessibility"
        menu: true
        checked: true
    }
    QsTile {
        id: expressive
        x: 20
        y: 140
        width: 164
        icon: "redshift-status-on"
        label: "Night light"
        checked: root.nightLight
        onToggled: if (!root.refuse) root.nightLight = !root.nightLight
    }
    QsTile {
        id: expressiveSplit
        x: 200
        y: 140
        width: 164
        icon: "network-wireless"
        label: "Wi-Fi"
        split: true
        checked: root.wifi
        onToggled: if (!root.refuse) root.wifi = !root.wifi
    }
    QsTile {
        id: expressiveMenu
        x: 380
        y: 140
        width: 164
        icon: "preferences-desktop-accessibility"
        label: "Accessibility"
        menu: true
        checked: true
    }

    TestCase {
        name: "QuickSettingsTileAccessibility"
        when: windowShown

        function init() {
            root.refuse = false
            root.nightLight = true
            root.wifi = false
            start.forceActiveFocus()
        }

        function heard(tile, half) {
            const state = AccessibleProbe.state(findChild(tile, half || "mainButton"))
            verify(state.valid, "no accessible interface")
            return state
        }

        function looks() {
            return [["default", plain, split, menu], ["expressive", expressive, expressiveSplit, expressiveMenu]]
        }

        function test_a_tile_is_a_toggle_button_with_its_state() {
            for (const [look, tile] of looks()) {
                const state = heard(tile)
                compare(state.role, Accessible.Button, look)
                compare(state.name, "Night light", look)
                verify(state.checkable, look)
                verify(state.checked, `${look}: on from the start`)
            }
        }

        function test_the_state_follows_the_tile() {
            for (const [look, tile] of looks()) {
                root.nightLight = false
                verify(!heard(tile).checked, look)
                root.nightLight = true
                verify(heard(tile).checked, look)
            }
        }

        function test_a_click_and_the_keyboard_change_it_through_the_owner() {
            for (const [look, tile] of looks()) {
                mouseClick(findChild(tile, "mainButton"))
                compare(root.nightLight, false, look)
                verify(!heard(tile).checked, look)
                findChild(tile, "mainButton").forceActiveFocus(Qt.TabFocusReason)
                keyClick(Qt.Key_Space)
                compare(root.nightLight, true, look)
                verify(heard(tile).checked, look)
                keyClick(Qt.Key_Return)
                compare(root.nightLight, false, look)
                verify(!heard(tile).checked, look)
                root.nightLight = true
            }
        }

        function test_a_refused_change_is_not_heard_as_done() {
            root.refuse = true
            for (const [look, tile] of looks()) {
                mouseClick(findChild(tile, "mainButton"))
                verify(root.nightLight, look)
                verify(heard(tile).checked, `${look}: Qt's own toggle stayed`)
                verify(tile.checked, look)
            }
        }

        function test_a_screen_reader_started_later_hears_the_same() {
            AccessibleProbe.restart()
            for (const [look, tile] of looks()) {
                const state = heard(tile)
                verify(state.checkable, look)
                verify(state.checked, look)
            }
        }

        function test_split_halves() {
            for (const [look, , tile] of looks()) {
                const main = heard(tile)
                compare(main.name, "Wi-Fi", look)
                verify(main.checkable, look)
                verify(!main.checked, look)
                const details = heard(tile, "detailsButton")
                compare(details.role, Accessible.Button, look)
                compare(details.name, "Wi-Fi settings", look)
                verify(!details.checkable, `${look}: the arrow opens a page`)
                mouseClick(findChild(tile, "mainButton"))
                verify(root.wifi, look)
                verify(heard(tile).checked, look)
                verify(!heard(tile, "detailsButton").checked, look)
                root.wifi = false
            }
        }

        function test_a_menu_tile_is_a_button() {
            for (const [look, , , tile] of looks()) {
                const state = heard(tile)
                compare(state.role, Accessible.Button, look)
                compare(state.name, "Accessibility", look)
                verify(!state.checkable, `${look}: it opens a page`)
            }
        }
    }
}
