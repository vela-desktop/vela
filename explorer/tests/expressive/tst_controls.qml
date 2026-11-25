// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../../qml"
import "../../../common/tests/colors.js" as Colors

// Files' controls under a theme with controls-variant=expressive (the
// files-views-expressive ctest entry): the tool buttons and the transfer
// cards in M3's shapes, colors and state layers, compared with the live
// Theme values, with their keyboard and accessibility.
Item {
    id: root
    width: 700
    height: 500

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    ToolButton { id: copy; x: 20; y: 20; icon.name: "edit-copy"; tooltip: "Copy" }
    ToolButton { id: pane; x: 80; y: 20; text: "Details"; checkable: true; checked: true }
    SignalSpy { id: clicks; target: copy; signalName: "clicked" }

    ProgressPanel {
        id: panel
        anchors { right: parent.right; bottom: parent.bottom; margins: 16 }
    }

    TestCase {
        name: "FilesControlsExpressive"
        when: windowShown

        function init() {
            if (!Theme.expressive)
                skip("with the expressive theme (the *-expressive ctest entry)")
            mouseMove(root, 690, 10)
            start.forceActiveFocus()
            clicks.clear()
            Ops.jobs = []
        }

        function stateLayer(amount) {
            return Qt.rgba(Theme.surfaceVariantText.r, Theme.surfaceVariantText.g, Theme.surfaceVariantText.b, amount)
        }

        function test_tool_button_shapes_and_state_layers() {
            verify(copy.expressive)
            compare(copy.implicitHeight, 32)
            compare(copy.leftPadding, 6)
            compare(pane.leftPadding, 16)
            tryCompare(copy.background, "radius", copy.height / 2, 5000)
            tryVerify(() => Colors.same(copy.background.color, stateLayer(0)), 5000)
            mouseMove(copy, 10, 10)
            tryVerify(() => Colors.same(copy.background.color, stateLayer(Theme.stateHover)), 5000)
            mousePress(copy, 10, 10)
            tryVerify(() => Colors.same(copy.background.color, stateLayer(Theme.statePressed)), 5000)
            tryCompare(copy.background, "radius", Theme.shapeSmall, 5000)
            mouseRelease(copy, 10, 10)
            compare(clicks.count, 1)
            // Checked: a square secondary container.
            tryCompare(pane.background, "radius", Theme.shapeMedium, 5000)
            tryVerify(() => Colors.same(pane.background.color, Theme.secondaryContainer), 5000)
        }

        function test_tool_button_tooltip_is_plain() {
            mouseMove(copy, 10, 10)
            const tip = findChild(copy, "tooltip")
            tryCompare(tip, "visible", true, 5000)
            compare(tip.radius, Theme.shapeExtraSmall)
            compare(tip.border.width, 0)
            verify(Colors.same(tip.color, Theme.inverseSurface))
            verify(Colors.same(tip.children[0].color, Theme.inverseSurfaceText))
        }

        function test_tool_button_keyboard_and_accessibility() {
            keyClick(Qt.Key_Tab)
            verify(copy.activeFocus)
            const ring = copy.background.children[0]
            verify(ring.visible)
            verify(Colors.same(ring.border.color, Theme.secondary))
            keyClick(Qt.Key_Space)
            keyClick(Qt.Key_Return)
            compare(clicks.count, 2)
            compare(copy.Accessible.name, "Copy")
        }

        function test_transfer_card() {
            Ops.jobs = [{ id: 7, title: "Copying", doneTitle: "Copied", done: 50, total: 100,
                          current: "a.txt", finished: false, error: "" }]
            tryVerify(() => findChild(panel, "transferJob:7") !== null, 5000)
            const card = findChild(panel, "transferJob:7")
            compare(card.radius, Theme.shapeMedium)
            compare(card.border.width, 0)
            const track = findChild(card, "transferTrack")
            compare(track.height, 4)
            verify(Colors.same(track.color, Theme.secondaryContainer))
            verify(Colors.same(track.children[0].color, Theme.primary))
            Ops.jobs = []
        }
    }
}
