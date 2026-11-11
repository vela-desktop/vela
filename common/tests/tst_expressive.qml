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

        // Without theme values the roles are the accent's Tonal Spot palette
        // (Style.tonal, vela-color's): the same hue family as the accent,
        // not Material's baseline purple, each with its own key.
        function test_roles_come_from_the_palette() {
            const roles = {
                primary: "primary", primaryText: "onPrimary",
                primaryContainer: "primaryContainer", primaryContainerText: "onPrimaryContainer",
                secondary: "secondary", secondaryText: "onSecondary",
                secondaryContainer: "secondaryContainer", secondaryContainerText: "onSecondaryContainer",
                tertiary: "tertiary", tertiaryText: "onTertiary",
                tertiaryContainer: "tertiaryContainer", tertiaryContainerText: "onTertiaryContainer",
                error: "error", errorText: "onError",
                errorContainer: "errorContainer", errorContainerText: "onErrorContainer",
                surfaceTone: "surface",
                surfaceContainerLowest: "surfaceContainerLowest", surfaceContainerLow: "surfaceContainerLow",
                surfaceContainer: "surfaceContainer", surfaceContainerHigh: "surfaceContainerHigh",
                surfaceContainerHighest: "surfaceContainerHighest",
                surfaceText: "onSurface", surfaceVariantText: "onSurfaceVariant",
                outline: "outline", outlineVariant: "outlineVariant",
                inverseSurface: "inverseSurface", inverseSurfaceText: "inverseOnSurface"
            }
            for (const name in roles)
                verify(Colors.same(Theme[name], Style.tonal[roles[name]]), name)
            // The palette's source is the accent (no theme here sets one or
            // a primary), in the current mode.
            const own = Style.tonalOf(Theme.accent, Theme.light)
            for (const key in own)
                verify(Colors.same(own[key], Style.tonal[key]), key)
            verify(!Colors.same(Theme.primary, Theme.tertiary))
        }

        // Material's pairs read in both modes: content on its role at 4.5:1
        // (WCAG's for body text), the outline on the surface at 3:1 (a
        // target's boundary).
        function test_palette_contrast_in_both_modes() {
            const pairs = [
                ["primary", "onPrimary"], ["primaryContainer", "onPrimaryContainer"],
                ["secondary", "onSecondary"], ["secondaryContainer", "onSecondaryContainer"],
                ["tertiary", "onTertiary"], ["tertiaryContainer", "onTertiaryContainer"],
                ["error", "onError"], ["errorContainer", "onErrorContainer"],
                ["surface", "onSurface"], ["surface", "onSurfaceVariant"],
                ["surfaceContainerLowest", "onSurface"], ["surfaceContainerLow", "onSurface"],
                ["surfaceContainer", "onSurface"], ["surfaceContainerHigh", "onSurface"],
                ["surfaceContainerHighest", "onSurface"], ["surfaceContainerHighest", "onSurfaceVariant"],
                ["inverseSurface", "inverseOnSurface"]
            ]
            for (const isLight of [true, false]) {
                const tonal = Style.tonalOf(Theme.accent, isLight)
                const mode = isLight ? "light " : "dark "
                for (const [role, on] of pairs)
                    verify(Theme.contrastOf(tonal[role], tonal[on]) >= 4.5, mode + on + " on " + role)
                verify(Theme.contrastOf(tonal.surface, tonal.outline) >= 3, mode + "outline on surface")
            }
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
