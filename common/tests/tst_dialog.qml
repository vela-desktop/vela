// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls

// Dialog: the buttons it is given, Esc and Enter, a disabled accent button,
// closing without choosing, and its panel's PanelMotion: it fades out and
// hides, and a reopening during that keeps it (docs/visual-coherence.md
// §6.9).
Item {
    id: root
    width: 800
    height: 600

    property bool valid: false

    Dialog {
        id: dialog
        title: "Title"
        text: "Text"
        buttons: [
            { text: "Save", accent: true, value: "save", enabled: root.valid },
            { text: "Cancel", value: "cancel" }
        ]
        Rectangle { id: extra; width: parent.width; height: 40 }
    }
    SignalSpy { id: chosen; target: dialog; signalName: "chosen" }

    TestCase {
        name: "Dialog"
        when: windowShown

        function init() {
            root.valid = false
            dialog.close()
            tryCompare(dialog, "visible", false, 3000)
            chosen.clear()
        }

        function button(value) {
            return findChild(dialog, "button:" + value)
        }

        function test_content_and_buttons() {
            dialog.open()
            verify(extra.visible)
            compare(extra.width, 460 - 48)
            compare(button("save").text, "Save")
            verify(button("save").accent)
            verify(!button("save").enabled)
            verify(!button("cancel").accent)
            compare(button("save").width, button("cancel").width)
            root.valid = true
            verify(button("save").enabled)
        }

        function test_escape_cancels_and_fades_out() {
            dialog.open()
            keyClick(Qt.Key_Escape)
            compare(chosen.count, 1)
            compare(chosen.signalArguments[0][0], "cancel")
            verify(dialog.closing)
            verify(dialog.visible, "still visible while it fades")
            tryCompare(dialog, "visible", false, 3000)
        }

        function test_enter_chooses_the_accent_button_only_when_usable() {
            dialog.open()
            keyClick(Qt.Key_Return)
            compare(chosen.count, 0)
            verify(!dialog.closing)
            root.valid = true
            keyClick(Qt.Key_Return)
            compare(chosen.count, 1)
            compare(chosen.signalArguments[0][0], "save")
        }

        function test_a_click_chooses() {
            dialog.open()
            wait(Theme.normal * 2)
            mouseClick(button("cancel"))
            compare(chosen.count, 1)
            compare(chosen.signalArguments[0][0], "cancel")
        }

        function test_close_without_a_value_does_not_choose() {
            dialog.open()
            dialog.close()
            tryCompare(dialog, "visible", false, 3000)
            compare(chosen.count, 0)
        }

        function test_reopened_while_closing_stays() {
            dialog.open()
            wait(Theme.normal)
            dialog.close("cancel")
            wait(Theme.normal / 3)
            dialog.open()
            verify(!dialog.closing)
            wait(Theme.normal * 2)
            verify(dialog.visible)
        }
    }
}
