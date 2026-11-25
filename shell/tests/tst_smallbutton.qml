// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Test
import "../qml"

// The notification center's small buttons (SmallButton.qml), and the toggle
// that "Do not disturb" is: on in the accent while its owner says so, and a
// screen reader hearing a toggle button with that state (from its
// accessible interface, AccessibleProbe), however it changes.
Item {
    id: root
    width: 300
    height: 120

    property bool silent: false
    property bool refuse: false

    SmallButton {
        id: toggle
        x: 20
        y: 20
        icon.name: "notifications"
        accent: root.silent
        toggle: true
        on: root.silent
        Accessible.name: "Do not disturb"
        onClicked: if (!root.refuse) root.silent = !root.silent
    }
    SmallButton {
        id: plain
        x: 80
        y: 20
        text: "Clear all"
    }

    TestCase {
        name: "SmallButton"
        when: windowShown

        function init() {
            root.silent = false
            root.refuse = false
        }

        function test_a_toggle_is_heard_with_its_state() {
            let state = AccessibleProbe.state(toggle)
            compare(state.role, Accessible.Button)
            compare(state.name, "Do not disturb")
            verify(state.checkable)
            verify(!state.checked)
            root.silent = true
            verify(AccessibleProbe.state(toggle).checked)
            AccessibleProbe.restart()
            verify(AccessibleProbe.state(toggle).checked, "a screen reader started later")
        }

        function test_a_click_and_space_change_it_through_the_owner() {
            mouseClick(toggle)
            verify(root.silent)
            verify(toggle.accent)
            verify(AccessibleProbe.state(toggle).checked)
            toggle.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            verify(!root.silent)
            verify(!AccessibleProbe.state(toggle).checked)
            verify(toggle.subtle, "off: subtle again")
        }

        function test_a_refused_change_is_not_heard_as_done() {
            root.refuse = true
            mouseClick(toggle)
            verify(!root.silent)
            verify(!toggle.checked)
            verify(!AccessibleProbe.state(toggle).checked)
        }

        function test_a_plain_one_is_a_button() {
            const state = AccessibleProbe.state(plain)
            compare(state.role, Accessible.Button)
            compare(state.name, "Clear all")
            verify(!state.checkable)
            verify(plain.subtle)
            compare(plain.implicitHeight, 28)
            compare(toggle.implicitWidth, 32)
        }
    }
}
