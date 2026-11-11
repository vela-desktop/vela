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
    Slider { id: thickSlider; x: 20; y: 260; from: 0; to: 100; value: 50; thick: true; styleVariant: "expressive" }
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
            verify(Colors.same(findChild(filled, "label").color, Theme.primaryText))
            filled.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Return)
            compare(clicks.count, 1)
        }

        // States are layers of the content color: hovered is part of the way
        // to onSecondaryContainer, not a fixed color from elsewhere.
        function test_button_hover_is_a_state_layer() {
            mouseMove(tonal, tonal.width / 2, tonal.height / 2)
            const hovered = Theme.mix(Theme.secondaryContainer, Theme.secondaryContainerText, 0.08)
            verify(!Colors.same(hovered, Theme.secondaryContainer))
            tryVerify(() => Colors.same(tonal.background.color, hovered), 2000)
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

        // Quick Settings' thick slider keeps its shape under the expressive
        // variant: a full-width track and the wide knob, only tonal colors.
        function test_thick_slider_keeps_its_shape() {
            verify(thickSlider.expressive)
            verify(!thickSlider.expressiveShape)
            compare(thickSlider.implicitHeight, 32)
            compare(thickSlider.handle.width, 40)
            compare(thickSlider.handle.height, 25)
            compare(thickSlider.background.children[0].x, 0)
            compare(thickSlider.background.children[0].width, thickSlider.availableWidth)
            verify(Colors.same(thickSlider.background.children[0].color, Theme.surfaceContainerHigh))
        }

        // Content on the tonal roles reads at 4.5:1 whatever the theme set.
        function test_on_roles_stay_legible() {
            verify(Theme.contrastOf(Theme.primary, Theme.primaryText) >= 4.5)
            verify(Theme.contrastOf(Theme.primaryContainer, Theme.primaryContainerText) >= 4.5)
            verify(Theme.contrastOf(Theme.secondaryContainer, Theme.secondaryContainerText) >= 4.5)
            // A theme's unreadable choice gives way to black or white.
            verify(Colors.same(Theme.legibleOn("#ffff00", "#ffffff"), Qt.color("#000000")))
            verify(Colors.same(Theme.legibleOn("#202020", "#303030"), Qt.color("#ffffff")))
            verify(Colors.same(Theme.legibleOn("#ffffff", "#1d192b"), Qt.color("#1d192b")))
        }

        // Without theme values the containers follow the accent: the same hue
        // family as primary, not Material's baseline purple.
        function test_fallback_containers_follow_primary() {
            verify(Colors.same(Theme.mix("#000000", "#ffffff", 0.5), Qt.rgba(0.5, 0.5, 0.5, 1)))
            const expected = Theme.light ? Theme.mix(Theme.primary, "#f0f0f0", 0.82)
                                         : Theme.mix(Theme.primary, "#2c2c2c", 0.8)
            verify(Colors.same(Theme.secondaryContainer, expected))
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
