// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The shell's controller, as far as its windows place themselves: the
// tests' `Shell` (shell_test.cpp). It records where a window was put.
QtObject {
    property string primaryScreen: "test-output"
    property var placed: []

    function placeOnScreen(window, name) { placed = placed.concat([name]) }
}
