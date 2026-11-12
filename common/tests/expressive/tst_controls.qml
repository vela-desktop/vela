// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../colors.js" as Colors

// Vela.Controls under a theme with controls-variant=expressive (the
// *-expressive ctest entries): the Fluent tokens take the tonal roles, and
// every control takes M3's shapes, sizes and state layers (the expectations
// are the live Theme values), with the same keyboard and accessibility.
Item {
    id: root
    width: 800
    height: 600

    Item { id: start; focus: true; activeFocusOnTab: true; width: 10; height: 10 }
    Button { id: tonal; x: 20; y: 20; text: "Tonal" }
    Button { id: filled; x: 140; y: 20; text: "Filled"; accent: true }
    Button { id: textButton; x: 260; y: 20; text: "Text"; subtle: true }
    Button { id: toggle; x: 380; y: 20; text: "Open"; subtle: true; checkable: true }
    Button { id: iconOnly; x: 500; y: 20; icon.name: "go-up"; subtle: true }
    Switch { id: control; x: 20; y: 90 }
    Slider { id: slider; x: 20; y: 150; from: 0; to: 100; value: 40 }
    ListItem { id: row; x: 20; y: 210; width: 240; text: "Row" }
    ListItem { id: current; x: 20; y: 270; width: 240; text: "Current"; checked: true }
    TextBox { id: field; x: 300; y: 210 }
    TonalSurface { id: surface; x: 300; y: 270; width: 100; height: 60 }
    SignalSpy { id: clicks; target: tonal; signalName: "clicked" }
    SignalSpy { id: rowClicks; target: row; signalName: "clicked" }

    Dialog {
        id: dialog
        title: "Delete the file?"
        text: "It goes to the trash."
        buttons: [
            { text: "Delete", accent: true, value: "delete" },
            { text: "Cancel", value: "cancel" }
        ]
    }
    SignalSpy { id: chosen; target: dialog; signalName: "chosen" }

    TestCase {
        name: "ControlsExpressive"
        when: windowShown

        function init() {
            if (!Theme.expressive)
                skip("with the expressive theme (the *-expressive ctest entries)")
            mouseMove(root, 790, 590)
            start.forceActiveFocus()
            clicks.clear()
            rowClicks.clear()
            toggle.checked = false
            dialog.close()
            tryCompare(dialog, "visible", false, 5000)
            chosen.clear()
        }

        // The Fluent tokens are the tonal roles (Theme.qml's mapping).
        function test_fluent_tokens_take_the_roles() {
            const map = {
                background: "surfaceTone", card: "surfaceContainerLow", control: "surfaceContainerHigh",
                flyout: "surfaceContainerHigh", popup: "surfaceContainerHigh", dialog: "surfaceContainerHigh",
                dialogFooter: "surfaceContainerHigh", surface: "surfaceContainer", taskbar: "surfaceContainer",
                backdrop: "surfaceContainer", taskCard: "surfaceContainer", field: "surfaceContainerHighest",
                text: "surfaceText", textSecondary: "surfaceVariantText", textTertiary: "outline",
                stroke: "outlineVariant", cardStroke: "outlineVariant", controlStroke: "outlineVariant",
                divider: "outlineVariant", layerStroke: "outlineVariant", controlStrokeStrong: "outline",
                focusRing: "outline", selection: "secondaryContainer", critical: "error",
                accentFill: "primary", accentText: "primaryText", link: "primary"
            }
            for (const token in map)
                verify(Colors.same(Theme[token], Theme[map[token]]), token + " is " + map[token])
            verify(Colors.same(Theme.cardHover, Theme.mix(Theme.surfaceContainerLow, Theme.surfaceText, Theme.stateHover)))
            verify(Colors.same(Theme.cardPressed, Theme.mix(Theme.surfaceContainerLow, Theme.surfaceText, Theme.statePressed)))
            compare(Theme.weightLabel, Font.Medium)
            compare(Theme.weightEmphasis, Font.Bold)
        }

        function test_button_shapes() {
            for (const b of [tonal, filled, textButton]) {
                verify(b.expressive, b.text)
                compare(b.implicitHeight, 40)
                compare(b.leftPadding, 16)
                compare(b.font.weight, Font.Medium)
                tryCompare(b.background, "radius", b.height / 2, 5000)
            }
            compare(iconOnly.width, 40)
            mousePress(tonal, 10, 10)
            tryCompare(tonal.background, "radius", Theme.shapeSmall, 5000)
            mouseRelease(tonal, 10, 10)
            tryCompare(tonal.background, "radius", tonal.height / 2, 5000)
            toggle.checked = true
            tryCompare(toggle.background, "radius", Theme.shapeMedium, 5000)
        }

        function test_button_radius_morphs_on_the_spatial_curve() {
            if (!Theme.animationEffects)
                skip("with effects on")
            if (!Theme.spatialMotion)
                skip("with motion-variant=expressive")
            mousePress(tonal, 10, 10)
            wait(Theme.spatialDuration("fast", Theme.fast) / 4)
            const r = tonal.background.radius
            verify(r < tonal.height / 2 && r > Theme.shapeSmall - 2, "on its way: " + r)
            mouseRelease(tonal, 10, 10)
        }

        function test_button_state_layers() {
            const cases = [
                [tonal, Theme.secondaryContainer, Theme.secondaryContainerText],
                [filled, Theme.primary, Theme.primaryText]
            ]
            for (const [b, fill, content] of cases) {
                tryVerify(() => Colors.same(b.background.color, fill), 5000, b.text + " at rest")
                mouseMove(b, 10, 10)
                tryVerify(() => Colors.same(b.background.color, Theme.mix(fill, content, Theme.stateHover)), 5000, b.text + " hovered")
                mousePress(b, 10, 10)
                tryVerify(() => Colors.same(b.background.color, Theme.mix(fill, content, Theme.statePressed)), 5000, b.text + " pressed")
                mouseRelease(b, 10, 10)
                mouseMove(root, 790, 590)
            }
            // A text button: no fill at rest, primary's layer under the pointer.
            compare(textButton.background.color.a, 0)
            verify(Colors.same(findChild(textButton, "label").color, Theme.primary))
            mouseMove(textButton, 10, 10)
            tryVerify(() => Math.abs(textButton.background.color.a - Theme.stateHover) < 0.01, 5000)
        }

        function test_button_keyboard_and_accessibility() {
            keyClick(Qt.Key_Tab)
            verify(tonal.activeFocus)
            const ring = tonal.background.children[0]
            verify(ring.visible)
            verify(Colors.same(ring.border.color, Theme.secondary))
            keyClick(Qt.Key_Space)
            keyClick(Qt.Key_Return)
            compare(clicks.count, 2)
            compare(tonal.Accessible.role, Accessible.Button)
            compare(tonal.Accessible.name, "Tonal")
        }

        // The ring reads on every surface the controls sit on (3:1).
        function test_focus_ring_contrast() {
            for (const isLight of [true, false]) {
                const t = Style.tonalOf(Theme.accent, isLight)
                for (const s of ["surface", "surfaceContainerLow", "surfaceContainer", "surfaceContainerHigh", "surfaceContainerHighest"])
                    verify(Theme.contrastOf(t.secondary, t[s]) >= 3, (isLight ? "light " : "dark ") + s)
            }
        }

        function test_switch_handle_and_state_layer() {
            const track = control.indicator
            const knob = track.children[0]
            const halo = track.children[2]
            compare(track.width, 52)
            compare(track.height, 32)
            compare(control.showLabel, false)
            tryVerify(() => Colors.same(track.color, Theme.surfaceContainerHighest), 5000)
            compare(track.border.width, 2)
            verify(Colors.same(track.border.color, Theme.outline))
            tryCompare(knob, "width", 16, 5000)
            verify(halo.visible)
            compare(halo.width, 40)
            compare(halo.color.a, 0)
            mouseMove(track, 10, 16)
            tryVerify(() => Math.abs(halo.color.a - Theme.stateHover) < 0.01, 5000)
            mousePress(track, 10, 16)
            tryCompare(knob, "width", 28, 5000)
            mouseRelease(track, 10, 16)
            verify(control.checked)
            tryCompare(knob, "width", 24, 5000)
            tryVerify(() => Colors.same(track.color, Theme.primary), 5000)
            verify(Colors.same(knob.color, Theme.primaryText))
            // The handle's center 16 from the end.
            tryVerify(() => Math.abs(knob.x + knob.width / 2 - (track.width - 16)) < 0.5, 5000)
            control.checked = false
        }

        function test_switch_keyboard_and_accessibility() {
            keyClick(Qt.Key_Tab) // tonal
            for (let i = 0; i < 5; ++i)
                keyClick(Qt.Key_Tab)
            verify(control.activeFocus)
            verify(control.indicator.children[1].visible)
            keyClick(Qt.Key_Space)
            verify(control.checked)
            verify(control.Accessible.checkable)
            compare(control.Accessible.checked, true)
            keyClick(Qt.Key_Space)
            verify(!control.checked)
        }

        function test_slider_colors() {
            verify(Colors.same(slider.background.children[0].color, Theme.secondaryContainer))
            verify(Colors.same(slider.background.children[2].color, Theme.primary))
            verify(Colors.same(slider.handle.color, Theme.primary))
            compare(slider.handle.width, 8)
        }

        function test_list_item_states_and_shapes() {
            compare(row.implicitHeight, 56)
            compare(row.leftPadding, 16)
            compare(row.background.radius, Theme.shapeMedium)
            compare(row.font.weight, Font.Normal)
            compare(row.background.color.a, 0)
            mouseMove(row, 20, 20)
            tryVerify(() => Colors.same(row.background.color,
                Qt.rgba(Theme.surfaceText.r, Theme.surfaceText.g, Theme.surfaceText.b, Theme.stateHover)), 5000)
            mousePress(row, 20, 20)
            tryVerify(() => Math.abs(row.background.color.a - Theme.statePressed) < 0.01, 5000)
            mouseRelease(row, 20, 20)
            compare(rowClicks.count, 1)
            // Selected: a full pill of secondary container, the label Medium.
            tryCompare(current.background, "radius", current.height / 2, 5000)
            tryVerify(() => Colors.same(current.background.color, Theme.secondaryContainer), 5000)
            compare(current.font.weight, Font.Medium)
        }

        function test_list_item_keyboard_and_accessibility() {
            row.forceActiveFocus(Qt.TabFocusReason)
            verify(row.background.children[0].visible)
            keyClick(Qt.Key_Return)
            keyClick(Qt.Key_Space)
            compare(rowClicks.count, 2)
            compare(row.Accessible.name, "Row")
        }

        function test_text_box_is_filled() {
            const bg = field.background
            const line = findChild(field, "indicator")
            compare(field.implicitHeight, 40)
            compare(bg.topLeftRadius, Theme.shapeExtraSmall)
            compare(bg.bottomLeftRadius, 0)
            compare(bg.border.width, 0)
            tryVerify(() => Colors.same(bg.color, Theme.surfaceContainerHighest), 5000)
            verify(Colors.same(line.color, Theme.surfaceVariantText))
            compare(line.height, 1)
            mouseMove(field, 20, 20)
            tryVerify(() => Colors.same(bg.color, Theme.mix(Theme.surfaceContainerHighest, Theme.surfaceText, Theme.stateHover)), 5000)
            field.forceActiveFocus()
            compare(line.height, 2)
            verify(Colors.same(line.color, Theme.primary))
            verify(Colors.same(field.selectionColor, Theme.primary))
            verify(Colors.same(field.selectedTextColor, Theme.primaryText))
            keyClick(Qt.Key_A)
            compare(field.text, "a")
            field.text = ""
        }

        function test_tonal_surface_levels() {
            const levels = { lowest: "surfaceContainerLowest", low: "surfaceContainerLow", container: "surfaceContainer",
                             high: "surfaceContainerHigh", highest: "surfaceContainerHighest" }
            for (const level in levels) {
                surface.level = level
                verify(Colors.same(surface.color, Theme[levels[level]]), level)
            }
            surface.level = Qt.binding(() => surface.highEmphasis ? "high" : "container")
            surface.highEmphasis = true
            verify(Colors.same(surface.color, Theme.surfaceContainerHigh))
            compare(surface.radius, Theme.shapeMedium)
            surface.highEmphasis = false
        }

        // M3's hierarchy is tonal: lower shadows than Fluent's.
        function test_shadows_are_lower() {
            compare(Theme.shadow.flyout.offset, 3)
            compare(Theme.shadow.dialog.offset, 6)
            verify(Theme.shadow.dialog.blur < 48)
        }

        function test_dialog_shape_and_actions() {
            dialog.open()
            tryCompare(dialog, "visible", true)
            const panel = findChild(dialog, "button:delete").parent.parent.parent
            compare(panel.radius, Theme.shapeExtraLarge)
            compare(panel.border.width, 0)
            const title = findChild(dialog, "title")
            compare(title.font.pixelSize, Math.round(Theme.fontBody * 24 / 14))
            compare(title.font.weight, Font.Normal)
            verify(Colors.same(findChild(dialog, "text").color, Theme.textSecondary))
            compare(findChild(dialog, "footer").color.a, 0)
            const confirm = findChild(dialog, "button:delete")
            const cancel = findChild(dialog, "button:cancel")
            // The confirming action at the trailing edge, tonal; the other a
            // text button, before it.
            verify(confirm.x > cancel.x)
            verify(!confirm.accent && !confirm.subtle)
            verify(cancel.subtle)
            compare(confirm.width, confirm.implicitWidth)
            keyClick(Qt.Key_Return)
            compare(chosen.count, 1)
            compare(chosen.signalArguments[0][0], "delete")
            tryCompare(dialog, "visible", false, 5000)
            dialog.open()
            keyClick(Qt.Key_Escape)
            compare(chosen.signalArguments[1][0], "cancel")
        }
    }
}
