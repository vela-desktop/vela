// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "colors.js" as Colors

// Test the opt-in built-in variant without changing the global theme.
// The existing control suites continue to guard the default Fluent look.
Item {
    id: root
    width: 420
    height: 320

    Button { id: filled; x: 20; y: 20; text: "Primary"; accent: true; styleVariant: "expressive" }
    Button { id: tonal; x: 20; y: 80; text: "Tonal"; styleVariant: "expressive" }
    Switch { id: toggle; x: 20; y: 140; styleVariant: "expressive" }
    Slider { id: slider; x: 20; y: 210; from: 0; to: 100; value: 50; styleVariant: "expressive" }
    TonalSurface { id: surface; x: 240; y: 30; width: 120; height: 90; outlined: true }
    SignalSpy { id: clicks; target: filled; signalName: "clicked" }

    TestCase {
        name: "ExpressiveControls"
        when: windowShown

        function init() {
            mouseMove(root, 400, 300)
            clicks.clear()
            toggle.checked = false
            surface.highEmphasis = false
        }

        function test_button_shapes_and_colors() {
            verify(filled.expressive)
            compare(filled.implicitHeight, 40)
            compare(filled.background.radius, filled.background.height / 2)
            tryVerify(() => Colors.same(filled.background.color, Theme.primary), 2000)
            tryVerify(() => Colors.same(tonal.background.color, Theme.secondaryContainer), 2000)
            verify(Colors.same(filled.contentItem.children[0].children[1].color, Theme.onPrimary))
            filled.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Return)
            compare(clicks.count, 1)
        }

        function test_switch_has_large_track_and_toggles() {
            compare(toggle.showLabel, false)
            compare(toggle.indicator.width, 52)
            compare(toggle.indicator.height, 32)
            tryVerify(() => Colors.same(toggle.indicator.color, Theme.surfaceContainerHigh), 2000)
            mouseClick(toggle)
            compare(toggle.checked, true)
            tryVerify(() => Colors.same(toggle.indicator.color, Theme.primary), 2000)
        }

        function test_slider_uses_tonal_track_and_thin_handle() {
            compare(slider.implicitHeight, 40)
            compare(slider.handle.width, 8)
            compare(slider.handle.height, 28)
            compare(slider.handle.border.width, 0)
            verify(Colors.same(slider.handle.color, Theme.primary))
        }

        function test_surface_uses_shape_and_outline_tokens() {
            compare(surface.radius, Theme.shapeLarge)
            verify(Colors.same(surface.color, Theme.surfaceContainer))
            compare(surface.border.width, 1)
            surface.highEmphasis = true
            compare(surface.radius, Theme.shapeMedium)
            verify(Colors.same(surface.color, Theme.surfaceContainerHigh))
        }
    }
}
