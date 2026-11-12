// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../../qml"
import Vela.Shell.Test as ShellTest
import "../../../common/tests/colors.js" as Colors
import "../notifications.js" as Data

// The shell's expressive pieces each on its own (VELA_TEST_THEME in
// common/tests/quicktest.h): the shell variants inside an otherwise default
// theme (shell-controls-expressive-shell, what themes/notifications/ and
// themes/startmenu/expressive.conf choose), and the expressive controls
// without them (shell-controls-expressive-controls). The variants keep
// their shapes and layout and take the colors of the controls in use.
Item {
    id: root
    width: 760
    height: 600

    readonly property QtObject styles: ShellTest.Styles

    QuickSettingsTile {
        id: tile
        x: 20
        y: 20
        style: root.styles.quickSettings
        expressiveWidth: 164
        icon: "redshift-status-on"
        label: "Night light"
        onToggled: checked = !checked
    }
    ListModel { id: history }
    NotificationList {
        id: list
        x: 380
        width: 360
        height: 600
        style: root.styles.notifications
        timeFormat: "HH:mm"
        model: history
    }
    Component {
        id: toastComponent
        NotificationToast {
            x: 20
            y: 200
            width: 340
            notificationId: 1
            appName: "Mail"
            icon: ""
            summary: "Ada"
            body: ""
            actions: []
            hasDefaultAction: false
            critical: false
        }
    }

    TestCase {
        name: "ShellMixed"
        when: windowShown

        readonly property bool shellAlone: root.styles.quickSettings.expressive && !Theme.expressive
        readonly property bool controlsAlone: Theme.expressive && !root.styles.quickSettings.expressive

        function init() {
            mouseMove(root, 750, 590)
            tile.checked = false
            history.clear()
            for (const n of Data.history())
                history.append(n)
            waitForRendering(list)
        }

        function shape() {
            return findChild(tile, "tileShape")
        }

        // --- the shell variants alone, Fluent's controls ---

        function test_shell_variants_alone() {
            if (!shellAlone)
                skip("with the shell variants alone")
            verify(root.styles.startMenu.expressive)
            verify(root.styles.notifications.expressive)
            // Their shapes without the expressive theme's corner scale.
            compare(root.styles.quickSettings.radius, Theme.shapeExtraLarge)
            compare(root.styles.quickSettings.tileRadius, 16)
            compare(root.styles.startMenu.radius, Theme.shapeExtraLarge)
            compare(root.styles.notifications.cardRadius, Theme.shapeLarge)
            // The panels keep the theme's surface (acrylic), not a tonal one.
            verify(Colors.same(root.styles.quickSettings.surface, Theme.surface))
            verify(!Colors.same(Theme.surface, Theme.surfaceContainer))
            // The taskbar keeps its outline: the controls are Fluent's.
            verify(root.styles.taskbar.stroke.a > 0)
        }

        function test_tile_takes_the_controls_colors() {
            if (!shellAlone)
                skip("with the shell variants alone")
            compare(tile.width, 164)
            compare(tile.height, 56)
            const s = shape()
            verify(s, "the expressive tile")
            tryCompare(s, "radius", 28, 5000)
            // Off: the default tile's control fill with the panels' outline.
            verify(Colors.same(s.color, Theme.control))
            compare(s.border.width, 1)
            verify(Colors.same(s.border.color, Theme.stroke))
            verify(Colors.same(findChild(tile, "tileLabel").color, Theme.text))
            // On: the accent, white on it, the outline gone, square corners.
            tile.checked = true
            tryVerify(() => Colors.same(s.color, Theme.accent), 5000)
            tryCompare(s.border.color, "a", 0, 5000)
            tryCompare(s, "radius", 16, 5000)
            tryVerify(() => Colors.same(findChild(tile, "tileLabel").color, Qt.color("white")), 5000)
            // A click still goes through the owner.
            mouseClick(findChild(tile, "mainButton"))
            verify(!tile.checked)
        }

        function test_grouped_notifications_and_a_default_toast() {
            if (!shellAlone)
                skip("with the shell variants alone")
            const c = Data.cards(list)
            const outer = root.styles.notifications.cardRadius
            const inner = Math.min(outer, Theme.shapeExtraSmall)
            compare([c[1].topLeftRadius, c[1].bottomLeftRadius], [outer, inner])
            compare([c[2].topLeftRadius, c[2].bottomLeftRadius], [inner, inner])
            compare([c[3].topLeftRadius, c[3].bottomLeftRadius], [inner, outer])
            compare(Data.gap(c[1], c[2]), 2)
            // The cards on the controls' fill, as the panel's other controls.
            verify(Colors.same(c[1].color, Theme.control))
            // Toasts follow the controls: Fluent's here.
            const t = createTemporaryObject(toastComponent, root)
            compare(t.radius, Theme.radiusOverlay)
            compare(t.border.width, 1)
        }

        // --- the expressive controls alone, the default shell ---

        function test_controls_alone() {
            if (!controlsAlone)
                skip("with controls-variant=expressive alone")
            verify(!root.styles.startMenu.expressive)
            verify(!root.styles.notifications.expressive)
            // The default tile, three to a row, in the tonal controls.
            compare(tile.height, 82)
            verify(!shape(), "not the expressive tile")
            const main = findChild(tile, "mainButton")
            tryVerify(() => Colors.same(main.background.color, Theme.control), 5000)
            verify(Colors.same(Theme.control, Theme.surfaceContainerHigh))
            // The list's cards one by one, 6 apart, the style's corners.
            const c = Data.cards(list)
            compare(c[2].topLeftRadius, root.styles.notifications.cardRadius)
            compare(Data.gap(c[1], c[2]), 6)
            // The toast is the expressive controls' one.
            const t = createTemporaryObject(toastComponent, root)
            compare(t.radius, Theme.shapeLarge)
            compare(t.border.width, 0)
            // M3's surfaces have no outline: the taskbar neither.
            compare(root.styles.taskbar.stroke.a, 0)
        }
    }
}
