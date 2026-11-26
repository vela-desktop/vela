// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The shell's controller, as far as its windows place themselves and hear
// the compositor: the tests' `Shell` (shell_test.cpp). It records where a
// window was put.
QtObject {
    property string primaryScreen: "test-output"
    // The display the user is on, as the compositor says (empty: the main one).
    property string activeScreen: ""
    property var placed: []
    signal capsLockChanged(bool on)

    function panelScreen() { return activeScreen !== "" ? activeScreen : primaryScreen }
    function placeOnScreen(window, name) { placed = placed.concat([name]) }
}
