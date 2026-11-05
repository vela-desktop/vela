// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Personalization's Undo (UndoBar.qml): what a click on a theme or a style
// just changed, with Undo for a few seconds; and the cards those clicks are
// on (PictureTile.qml).
Item {
    id: root
    width: 900
    height: 700

    property var undone: []

    UndoBar {
        id: bar
        width: implicitWidth
        height: implicitHeight
        timeout: 3000
    }

    PictureTile {
        id: tile
        y: 100
        width: 240
        text: "Window Glass"
        subtitle: "Original author: Lowell137"
    }
    SignalSpy { id: clicks; target: tile; signalName: "clicked" }

    TestCase {
        name: "UndoBar"
        when: windowShown

        function buttons() {
            const found = []
            const walk = item => {
                for (let i = 0; i < item.children.length; ++i) {
                    const child = item.children[i]
                    if (child.text !== undefined && child.clicked !== undefined && child.hasOwnProperty("accent")) {
                        found.push(child)
                    }
                    walk(child)
                }
            }
            walk(bar)
            return found
        }

        // Offered, and in sight: before its fade-in ends a click may miss it.
        function offer(text, undo) {
            bar.offer(text, undo)
            tryCompare(bar, "opacity", 1)
        }

        function init() {
            bar.timeout = 3000
            bar.dismiss()
            root.undone = []
            clicks.clear()
            tryCompare(bar, "visible", false)
        }

        function test_undo_runs_once_and_goes_away() {
            offer("Window Glass is in use", () => { root.undone = root.undone.concat(["theme"]) })
            verify(bar.shown)
            const undo = buttons().find(b => b.text === "Undo")
            verify(undo)
            mouseClick(undo)
            compare(root.undone, ["theme"])
            verify(!bar.shown)
            // Gone: a second click can't undo twice.
            tryCompare(bar, "visible", false)
        }

        function test_a_new_offer_replaces_the_old_one() {
            offer("Pill is in use", () => { root.undone = root.undone.concat(["first"]) })
            offer("Dock is in use", () => { root.undone = root.undone.concat(["second"]) })
            compare(bar.text, "Dock is in use")
            mouseClick(buttons().find(b => b.text === "Undo"))
            compare(root.undone, ["second"])
        }

        function test_it_goes_away_by_itself_unless_hovered() {
            offer("Pill is in use", () => {})
            bar.timeout = 300
            mouseMove(bar, bar.width / 2, bar.height / 2)
            wait(bar.timeout * 2)
            verify(bar.shown, "it went away under the pointer")
            mouseMove(root, root.width - 1, root.height - 1)
            tryCompare(bar, "shown", false, bar.timeout * 4)
            compare(root.undone, [])
        }

        function test_close_doesnt_undo() {
            offer("Pill is in use", () => { root.undone = root.undone.concat(["undone"]) })
            const close = buttons().find(b => b.text === "")
            verify(close)
            mouseClick(close)
            verify(!bar.shown)
            compare(root.undone, [])
        }

        function test_tile_is_a_checkable_choice() {
            compare(tile.Accessible.role, Accessible.RadioButton)
            verify(!tile.Accessible.checked)
            tile.selected = true
            verify(tile.Accessible.checked)
            mouseClick(tile)
            compare(clicks.count, 1)
            // Keyboard: Tab gives it the focus, Space and Enter choose it.
            tile.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            keyClick(Qt.Key_Return)
            compare(clicks.count, 3)
            tile.selected = false
        }
    }
}
