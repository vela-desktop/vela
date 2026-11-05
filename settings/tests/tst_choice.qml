// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Choice.qml: a disabled drop-down looks disabled, all of it (Taskbar's
// alignment while a pill places the apps itself), and doesn't open.
Item {
    width: 400
    height: 300

    Choice {
        id: choice
        x: 20
        y: 20
        model: ["Center", "Left"]
    }

    TestCase {
        name: "Choice"
        when: windowShown

        function cleanup() {
            choice.usable = true
            choice.popup.close()
        }

        function test_disabled_fades_text_and_chevron_too() {
            compare(choice.opacity, 1)
            choice.usable = false
            verify(!choice.enabled)
            // The whole control, not only its background: the text and the
            // chevron are its children.
            compare(choice.opacity, 0.45)
            mouseClick(choice)
            verify(!choice.popup.visible, "a disabled drop-down opened")
        }

        function test_enabled_opens() {
            mouseClick(choice)
            tryCompare(choice.popup, "visible", true)
        }
    }
}
