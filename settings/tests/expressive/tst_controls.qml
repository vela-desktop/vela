// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../../qml"
import "../../../common/tests/colors.js" as Colors

// Settings' controls under a theme with controls-variant=expressive (the
// settings-controls-expressive ctest entries): cards as one container in a
// group, the drop-down, radio buttons, the level bar, the "..." menu, the
// snackbar and the picture tiles in M3's shapes, colors and state layers,
// compared with the live Theme values, with their keyboard and
// accessibility.
Item {
    id: root
    width: 900
    height: 700

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }

    CardGroup {
        id: group
        x: 20
        y: 20
        width: 400
        title: "Group"
        Card { id: top; title: "Top"; clickable: true }
        Card { id: middle; title: "Middle" }
        Card { id: hidden; title: "Hidden"; visible: false }
        Card { id: bottom; title: "Bottom" }
    }
    Card { id: alone; x: 20; y: 300; width: 400; title: "Alone" }

    Choice { id: choice; x: 450; y: 20; model: ["Center", "Left"] }
    RadioButton { id: radio; x: 450; y: 80; text: "Light" }
    LevelBar { id: level; x: 450; y: 130; level: 0.5 }
    MoreMenu { id: more; x: 450; y: 160; entries: [{ text: "Remove", action: () => {} }] }
    PictureTile { id: tile; x: 450; y: 220; text: "Vela"; selected: true }
    PageHeader { id: header; x: 20; y: 380; crumbs: [{ name: "system", title: "System" }] }
    UndoBar { id: bar; x: 20; y: 600 }

    TestCase {
        name: "SettingsControlsExpressive"
        when: windowShown

        function init() {
            if (!Theme.expressive)
                skip("with the expressive theme (the *-expressive ctest entries)")
            mouseMove(root, 890, 690)
            start.forceActiveFocus()
        }

        function test_card_group_is_one_container() {
            compare(top.parent.spacing, 2)
            compare(top.border.width, 0)
            verify(Colors.same(top.color, Theme.surfaceContainerLow))
            compare(top.topLeftRadius, Theme.shapeLarge)
            compare(top.bottomLeftRadius, Theme.shapeExtraSmall)
            compare(middle.topLeftRadius, Theme.shapeExtraSmall)
            compare(middle.bottomRightRadius, Theme.shapeExtraSmall)
            // The hidden card doesn't count: the last visible one is the end.
            compare(bottom.topLeftRadius, Theme.shapeExtraSmall)
            compare(bottom.bottomLeftRadius, Theme.shapeLarge)
            hidden.visible = true
            compare(bottom.topLeftRadius, Theme.shapeExtraSmall)
            hidden.visible = false
            // A card outside a group: large corners all round.
            compare(alone.topLeftRadius, Theme.shapeLarge)
            compare(alone.bottomRightRadius, Theme.shapeLarge)
        }

        function test_card_states_keyboard_and_accessibility() {
            mouseMove(top, 100, 20)
            tryVerify(() => Colors.same(top.color, Theme.cardHover), 5000)
            verify(Colors.same(Theme.cardHover, Theme.mix(Theme.surfaceContainerLow, Theme.surfaceText, Theme.stateHover)))
            const spy = createTemporaryObject(spyComponent, root, { target: top, signalName: "clicked" })
            keyClick(Qt.Key_Tab)
            const ring = top.children[top.children.length - 1]
            verify(ring.visible)
            compare(ring.topLeftRadius, Theme.shapeLarge + 3)
            keyClick(Qt.Key_Return)
            compare(spy.count, 1)
            compare(group.children[0].font.weight, Theme.weightEmphasis)
        }

        function test_choice_is_a_filled_field() {
            const bg = choice.background
            const line = findChild(choice, "indicator")
            compare(choice.implicitHeight, 40)
            compare(bg.topLeftRadius, Theme.shapeExtraSmall)
            compare(bg.bottomLeftRadius, 0)
            compare(bg.border.width, 0)
            tryVerify(() => Colors.same(bg.color, Theme.surfaceContainerHighest), 5000)
            verify(Colors.same(line.color, Theme.surfaceVariantText))
            mouseMove(choice, 20, 20)
            tryVerify(() => Colors.same(bg.color, Theme.mix(Theme.surfaceContainerHighest, Theme.surfaceText, Theme.stateHover)), 5000)
            mouseClick(choice)
            tryCompare(choice.popup, "visible", true, 5000)
            verify(Colors.same(line.color, Theme.primary))
            compare(line.height, 2)
            compare(choice.popup.background.border.width, 0)
            keyClick(Qt.Key_Down)
            keyClick(Qt.Key_Return)
            compare(choice.currentIndex, 1)
            tryCompare(choice.popup, "visible", false, 5000)
        }

        function test_radio_button() {
            const circle = radio.indicator
            const dot = circle.children[0]
            const layer = findChild(radio, "stateLayer")
            compare(circle.width, 20)
            compare(circle.border.width, 2)
            verify(Colors.same(circle.border.color, Theme.surfaceVariantText))
            mouseMove(radio, 10, 16)
            tryVerify(() => Math.abs(layer.color.a - Theme.stateHover) < 0.01, 5000)
            mouseClick(radio, 10, 16)
            verify(radio.checked)
            verify(dot.visible)
            compare(dot.width, 10)
            verify(Colors.same(dot.color, Theme.primary))
            verify(Colors.same(circle.border.color, Theme.primary))
            radio.checked = false
            radio.forceActiveFocus(Qt.TabFocusReason)
            verify(circle.children[1].visible)
            keyClick(Qt.Key_Space)
            verify(radio.checked)
            compare(radio.Accessible.role, Accessible.RadioButton)
            radio.checked = false
        }

        function test_level_bar() {
            verify(Colors.same(level.children[0].color, Theme.secondaryContainer))
            compare(level.children[0].opacity, 1)
            verify(Colors.same(level.children[1].color, Theme.primary))
        }

        function test_more_menu() {
            mouseClick(more)
            const menu = findChild(more, "menu")
            tryCompare(menu, "visible", true, 5000)
            compare(menu.background.border.width, 0)
            const entry = findChild(menu.contentItem, "entry:Remove")
            verify(entry.expressive)
            verify(entry.width >= entry.contentItem.implicitWidth + entry.leftPadding + entry.rightPadding)
            keyClick(Qt.Key_Escape)
            tryCompare(menu, "visible", false, 5000)
        }

        function test_picture_tile() {
            compare(tile.background.radius, Theme.shapeMedium)
            verify(Colors.same(tile.background.border.color, Theme.primary))
            compare(tile.Accessible.role, Accessible.RadioButton)
        }

        function test_page_header_weight() {
            const crumb = header.children[0].children[0]
            compare(crumb.font.weight, Font.Bold)
        }

        function test_undo_bar_is_a_snackbar() {
            bar.offer("Theme applied", () => {})
            tryCompare(bar, "opacity", 1, 5000)
            const panel = findChild(bar, "undoPanel")
            compare(panel.radius, Theme.shapeExtraSmall)
            compare(panel.border.width, 0)
            verify(Colors.same(panel.color, Theme.inverseSurface))
            const undo = findChild(bar, "undoButton")
            verify(undo.subtle)
            verify(Colors.same(findChild(undo, "label").color, Theme.inversePrimary))
            verify(findChild(bar, "closeMark").visible)
            compare(findChild(bar, "closeButton").Accessible.name, "Close")
            mouseClick(undo)
            verify(!bar.shown)
        }

        Component { id: spyComponent; SignalSpy {} }
    }
}
