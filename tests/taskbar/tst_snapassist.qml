// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Taskbar.Test

Item {
    Component { id: assistComponent; SnapAssist { width: 1280; height: 720 } }
    TestCase {
        name: "SnapAssistLifecycle"
        when: windowShown
        property var assist

        function init() {
            assist = createTemporaryObject(assistComponent, null)
            verify(assist)
            Shell.snapAssistRequested(JSON.stringify({window: "origin", output: "",
                tile: [0, 0, 6, 12], occupied: [], candidates: ["first", "second"]}))
            tryCompare(assist, "visible", true)
        }
        function cleanup() { assist.close() }

        function test_origin_closing_dismisses_exclusive_panel() {
            Capture.windowClosed("origin")
            compare(assist.visible, false)
            compare(assist.origin, "")
            compare(assist.candidates.length, 0)
            compare(assist.zones.length, 0)
        }
        function test_candidates_are_removed_until_none_remain() {
            Capture.windowClosed("first")
            compare(assist.visible, true)
            compare(assist.candidates.length, 1)
            compare(assist.candidates[0].id, "second")
            Capture.windowClosed("second")
            compare(assist.visible, false)
        }
        function test_unrelated_window_closing_keeps_panel() {
            Capture.windowClosed("unrelated")
            compare(assist.visible, true)
            compare(assist.candidates.length, 2)
        }
    }
}
