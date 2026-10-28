// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Bluetooth
import Vela.Controls
import "../qml"

// The Bluetooth page's "still visible" line (docs/bluetooth.md §6.6, rule
// 3, §9.1): shown while the daemon says so, with "Stop being visible".
Item {
    id: root
    width: 900
    height: 700

    StillVisibleCard {
        id: page
        width: 700
    }

    TestCase {
        name: "StillVisibleCard"
        when: windowShown

        function find(name) { return name === "stillVisible" ? page : findChild(page, name) }

        function init() {
            Bluetooth.reset()
        }

        function test_still_visible() {
            verify(!find("stillVisible").visible)
            Bluetooth.stillVisible = true
            verify(find("stillVisible").visible)
            compare(find("stillVisible").title, "Your PC is still visible to nearby devices")
            mouseClick(find("stopBeingVisible"))
            compare(Bluetooth.calls, ["stopBeingVisible"])
            // Nothing to press from the background.
            Bluetooth.active = false
            verify(!find("stopBeingVisible").enabled)
            Bluetooth.available = false
            verify(!find("stillVisible").visible)
        }
    }
}
