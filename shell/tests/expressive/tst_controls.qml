// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../../qml"
import "../../../common/tests/colors.js" as Colors

// The shell's controls under a theme with controls-variant=expressive (the
// shell-controls-expressive ctest entries): the check box, a menu's rows
// and the confirmation dialog in M3's shapes, colors and state layers,
// compared with the live Theme values, with their keyboard and
// accessibility.
Item {
    id: root
    width: 700
    height: 500

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    CheckBox { id: check; x: 20; y: 20; text: "Read-only" }

    Item {
        id: area
        x: 0
        y: 60
        width: 400
        height: 400
        MenuPanel {
            id: menu
            anchorX: 20
            anchorY: 20
            entries: [
                { text: "Open", icon: "document-open" },
                { text: "Show hidden files", checked: true },
                { separator: true },
                { text: "Delete" }
            ]
        }
    }
    SignalSpy { id: activated; target: menu; signalName: "activated" }

    TaskbarButton { id: taskButton; x: 450; y: 20; running: true }
    DesktopOsd { id: desktopOsd }
    VolumeOsd { id: volumeOsd }
    NotificationPopups { id: popups }
    TaskbarTooltip { id: tooltip }
    ConfirmDialog { id: confirm; title: "Empty the Recycle Bin?"; text: "The files go for good." }

    TestCase {
        name: "ShellControlsExpressive"
        when: windowShown

        function init() {
            if (!Theme.expressive)
                skip("with the expressive theme (the *-expressive ctest entries)")
            mouseMove(root, 690, 490)
            start.forceActiveFocus()
            check.checked = false
            activated.clear()
        }

        function test_check_box_shape_and_colors() {
            const box = check.indicator
            const mark = box.children[0]
            const layer = findChild(check, "stateLayer")
            verify(check.expressive)
            compare(box.width, 18)
            compare(box.radius, 2)
            compare(box.border.width, 2)
            verify(Colors.same(box.border.color, Theme.surfaceVariantText))
            compare(box.color.a, 0)
            verify(layer.visible)
            compare(layer.width, 40)
            mouseMove(check, 9, 11)
            tryVerify(() => Colors.same(layer.color, Qt.rgba(Theme.surfaceText.r, Theme.surfaceText.g, Theme.surfaceText.b, Theme.stateHover)), 5000)
            mousePress(check, 9, 11)
            tryVerify(() => Math.abs(layer.color.a - Theme.statePressed) < 0.01, 5000)
            mouseRelease(check, 9, 11)
            verify(check.checked)
            verify(mark.visible)
            tryVerify(() => Colors.same(box.color, Theme.primary), 5000)
            compare(box.border.width, 0)
            verify(Colors.same(mark.data[0].strokeColor, Theme.primaryText))
        }

        function test_check_box_keyboard_and_accessibility() {
            keyClick(Qt.Key_Tab)
            verify(check.activeFocus)
            const ring = check.indicator.children[1]
            verify(ring.visible)
            verify(Colors.same(ring.border.color, Theme.secondary))
            keyClick(Qt.Key_Space)
            verify(check.checked)
            compare(check.Accessible.role, Accessible.CheckBox)
            compare(check.Accessible.name, "Read-only")
        }

        function fills() {
            const list = []
            const walk = item => {
                for (let i = 0; i < item.children.length; ++i) {
                    if (item.children[i].objectName === "rowFill")
                        list.push(item.children[i])
                    walk(item.children[i])
                }
            }
            walk(menu)
            return list
        }

        function test_menu_rows() {
            tryCompare(menu, "opacity", 1, 5000)
            const [open, hidden, del] = fills()
            // At rest: nothing; the checked entry in tertiary container.
            compare(open.color.a, 0)
            compare(open.radius, Theme.shapeExtraSmall)
            compare(hidden.radius, Theme.shapeMedium)
            tryVerify(() => Colors.same(hidden.color, Theme.tertiaryContainer), 5000)
            // The current row: a state layer of on surface.
            mouseMove(open, 30, 16)
            tryVerify(() => Colors.same(open.color, Qt.rgba(Theme.surfaceText.r, Theme.surfaceText.g, Theme.surfaceText.b, Theme.stateHover)), 5000)
            mousePress(open, 30, 16)
            tryVerify(() => Math.abs(open.color.a - Theme.statePressed) < 0.01, 5000)
            mouseRelease(open, 30, 16)
            compare(activated.count, 1)
            compare(activated.signalArguments[0][1], 0)
            mouseMove(hidden, 30, 16)
            tryVerify(() => Colors.same(hidden.color, Theme.mix(Theme.tertiaryContainer, Theme.tertiaryContainerText, Theme.stateHover)), 5000)
            verify(del !== undefined)
        }

        function test_menu_keyboard() {
            menu.forceActiveFocus()
            menu.currentIndex = -1
            keyClick(Qt.Key_Down)
            compare(menu.currentIndex, 0)
            keyClick(Qt.Key_Down)
            compare(menu.currentIndex, 1)
            keyClick(Qt.Key_Down)
            compare(menu.currentIndex, 3, "the separator is skipped")
            keyClick(Qt.Key_Return)
            compare(activated.count, 1)
        }

        // Vela's choice: a 4 high pill, 24 wide in primary when active, 8
        // in outline when open.
        function test_taskbar_indicator() {
            const indicator = findChild(taskButton, "indicator")
            compare(indicator.height, 4)
            tryCompare(indicator, "width", 8, 5000)
            tryVerify(() => Colors.same(indicator.color, Theme.outline), 5000)
            taskButton.active = true
            tryCompare(indicator, "width", 24, 5000)
            tryVerify(() => Colors.same(indicator.color, Theme.primary), 5000)
            taskButton.active = false
        }

        function test_osds_are_pills() {
            for (const [osd, name] of [[desktopOsd, "desktopPill"], [volumeOsd, "volumePill"]]) {
                const pill = findChild(osd.contentItem, name)
                compare(pill.radius, pill.height / 2, name)
                compare(pill.border.width, 0, name)
                verify(Colors.same(pill.color, Theme.surfaceContainer), name)
            }
            // The volume bar: the thick slider's track in M3's roles.
            const bar = findChild(volumeOsd.contentItem, "volumeBar")
            compare(bar.children[0].height, 10)
            verify(Colors.same(bar.children[0].color, Theme.secondaryContainer))
            compare(bar.children[2].width, 40)
            compare(bar.children[2].height, 25)
            verify(Colors.same(bar.children[2].color, Theme.primary))
            compare(bar.children[2].border.width, 0)
        }

        function test_tooltip_is_plain() {
            const panel = findChild(tooltip.contentItem, "tooltip")
            compare(panel.radius, Theme.shapeExtraSmall)
            compare(panel.border.width, 0)
            verify(Colors.same(panel.color, Theme.inverseSurface))
            verify(Colors.same(panel.children[0].color, Theme.inverseSurfaceText))
        }

        function test_confirm_dialog() {
            const panel = findChild(confirm, "confirmPanel")
            compare(panel.radius, Theme.shapeExtraLarge)
            compare(panel.border.width, 0)
            verify(Colors.same(panel.color, Theme.surfaceContainerHigh))
            const title = findChild(confirm, "confirmTitle")
            compare(title.font.pixelSize, Math.round(Theme.fontBody * 24 / 14))
            verify(Colors.same(findChild(confirm, "confirmText").color, Theme.textSecondary))
            const yes = findChild(confirm, "confirmYes")
            const no = findChild(confirm, "confirmNo")
            verify(yes.expressive)
            verify(!yes.accent && !yes.subtle, "tonal")
            verify(no.subtle, "a text button")
            verify(yes.x > no.x, "Yes at the trailing edge")
            compare(yes.Accessible.name, yes.text)
        }
    }
}
