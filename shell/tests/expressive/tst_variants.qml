// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../../qml"
import Vela.Shell.Test as ShellTest
import "../../../common/tests/colors.js" as Colors

// The shell's expressive variants (quicksettings-variant=expressive, in
// the shell-controls-expressive test theme): the quick settings tile that
// changes shape (QsTile.qml), compared with the live Theme and Styles
// values, with the default tile's keyboard, right click and accessibility.
Item {
    id: root
    width: 500
    height: 400

    // The shell's Styles singleton (shell_test.cpp registers it).
    readonly property QtObject styles: ShellTest.Styles
    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    QsTile {
        id: plain
        x: 20
        y: 20
        width: 164
        squareRadius: styles.quickSettings.tileRadius
        icon: "redshift-status-on"
        label: "Night light"
        onToggled: checked = !checked
    }
    QsTile {
        id: split
        x: 200
        y: 20
        width: 164
        squareRadius: styles.quickSettings.tileRadius
        icon: "network-wireless"
        label: "Wi-Fi"
        split: true
    }
    QsTile {
        id: menu
        x: 20
        y: 100
        width: 164
        squareRadius: styles.quickSettings.tileRadius
        icon: "preferences-desktop-accessibility"
        label: "Accessibility"
        menu: true
    }
    SignalSpy { id: toggled; target: plain; signalName: "toggled" }
    SignalSpy { id: splitToggled; target: split; signalName: "toggled" }
    SignalSpy { id: splitDetails; target: split; signalName: "details" }
    SignalSpy { id: menuDetails; target: menu; signalName: "details" }
    SignalSpy { id: menuToggled; target: menu; signalName: "toggled" }
    SignalSpy { id: context; target: plain; signalName: "contextMenu" }

    TestCase {
        name: "ShellVariantsExpressive"
        when: windowShown

        function init() {
            if (!Theme.expressive)
                skip("with the expressive theme (the *-expressive ctest entries)")
            mouseMove(root, 490, 390)
            start.forceActiveFocus()
            plain.checked = false
            split.checked = false
            for (const spy of [toggled, splitToggled, splitDetails, menuDetails, menuToggled, context])
                spy.clear()
            tryCompare(plain, "radius", plain.height / 2, 5000)
        }

        function test_variants_are_on() {
            verify(styles.quickSettings.expressive)
            // The variant's defaults, unless the keys say otherwise.
            verify(styles.quickSettings.borderless)
            verify(styles.quickSettings.flatFooter)
            verify(styles.quickSettings.thickSlider)
            verify(!styles.quickSettings.inlineArrow)
            compare(styles.quickSettings.radius, Theme.shapeExtraLarge)
            compare(styles.quickSettings.tileRadius, 16)
            // Nested corners: the tile's square ones are the panel's less its
            // 12 of padding.
            compare(styles.quickSettings.radius - styles.quickSettings.tileRadius, 12)
        }

        function test_off_is_a_pill_on_is_primary_and_square() {
            const shape = findChild(plain, "tileShape")
            compare(plain.height, 56)
            compare(shape.radius, 28)
            verify(Colors.same(shape.color, Theme.surfaceContainerHigh))
            plain.checked = true
            tryVerify(() => Colors.same(shape.color, Theme.primary), 5000)
            tryCompare(shape, "radius", styles.quickSettings.tileRadius, 5000)
            verify(Colors.same(findChild(plain, "tileLabel").color, Theme.primaryText))
            plain.checked = false
            tryCompare(shape, "radius", 28, 5000)
        }

        function test_the_radius_morphs() {
            if (Theme.motion === 0)
                skip("without animation effects nothing moves")
            plain.checked = true
            wait(Theme.fast / 4)
            verify(plain.radius < 28 && plain.radius > 0, `the corners jumped: ${plain.radius}`)
            tryCompare(plain, "radius", styles.quickSettings.tileRadius, 5000)
        }

        function test_without_animation_effects_the_shape_changes_at_once() {
            if (Theme.motion !== 0)
                skip("with animation effects the corners morph")
            plain.checked = true
            compare(plain.radius, styles.quickSettings.tileRadius)
        }

        function test_pressed_shape_and_state_layer() {
            const layer = findChild(plain, "mainLayer")
            mouseMove(plain, 60, 28)
            tryVerify(() => Math.abs(layer.color.a - Theme.stateHover) < 0.01, 5000)
            mousePress(plain, 60, 28)
            tryCompare(plain, "radius", Theme.shapeMedium, 5000)
            tryVerify(() => Math.abs(layer.color.a - Theme.statePressed) < 0.01, 5000)
            mouseRelease(plain, 60, 28)
            compare(toggled.count, 1)
            verify(plain.checked)
            tryCompare(plain, "radius", styles.quickSettings.tileRadius, 5000)
        }

        function test_keyboard_and_accessibility() {
            const main = findChild(plain, "mainButton")
            keyClick(Qt.Key_Tab)
            verify(main.activeFocus)
            keyClick(Qt.Key_Return)
            compare(toggled.count, 1)
            keyClick(Qt.Key_Enter)
            compare(toggled.count, 2)
            // The default tile's names; its checkable and checked are set
            // as the default tile's (Qt's button writes its own over them
            // once a screen reader is there, in both looks).
            compare(main.Accessible.name, "Night light")
            compare(main.Accessible.role, Accessible.Button)
        }

        function test_split_tile_has_its_arrow() {
            const main = findChild(split, "mainButton")
            const details = findChild(split, "detailsButton")
            verify(details.visible)
            compare(details.width, 44)
            compare(main.width + details.width, split.width)
            compare(details.Accessible.name, "Wi-Fi settings")
            mouseClick(split, split.width - 20, 28)
            compare(splitDetails.count, 1)
            compare(splitToggled.count, 0)
            mouseClick(split, 40, 28)
            compare(splitToggled.count, 1)
            details.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Return)
            compare(splitDetails.count, 2)
        }

        function test_menu_tile_opens_its_page() {
            const main = findChild(menu, "mainButton")
            verify(!findChild(menu, "detailsButton").visible)
            mouseClick(menu, 60, 28)
            compare(menuDetails.count, 1)
            compare(menuToggled.count, 0)
        }

        function test_right_click_asks_for_settings() {
            mouseClick(plain, 60, 28, Qt.RightButton)
            compare(context.count, 1)
            compare(toggled.count, 0)
            plain.usable = false
            mouseClick(plain, 60, 28, Qt.RightButton)
            compare(context.count, 1)
            mouseClick(plain, 60, 28)
            compare(toggled.count, 0)
            plain.usable = true
        }

    }
}
