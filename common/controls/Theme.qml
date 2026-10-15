// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton

import QtQuick

// Vela's visual language, one for the shell and the apps: Windows 11's
// (Fluent) colors and sizes, light or dark, with the accent the user chose.
// The names follow WinUI's resources (TextFillColorSecondary is
// textSecondary, ControlCornerRadius is radius...), so a value can be checked
// against Microsoft's. Light or dark is Style's: the shell's mode for system
// UI, the apps' mode for apps (vela::controls::install).
QtObject {
    readonly property bool light: Style.light

    // --- the window: Mica, the same as Vela's title bar ---
    property bool windowActive: true // the app's Main.qml updates it
    readonly property color background: windowActive ? Style.mica : Style.micaInactive

    // --- controls and surfaces ---
    readonly property color control: light ? Qt.rgba(1, 1, 1, 0.7) : Qt.rgba(1, 1, 1, 0.061)
    readonly property color controlHover: light ? Qt.rgba(0.976, 0.976, 0.976, 0.5) : Qt.rgba(1, 1, 1, 0.084)
    readonly property color controlPressed: light ? Qt.rgba(0.976, 0.976, 0.976, 0.3) : Qt.rgba(1, 1, 1, 0.033)
    readonly property color controlStroke: light ? Qt.rgba(0, 0, 0, 0.07) : Qt.rgba(1, 1, 1, 0.07)
    readonly property color controlStrokeStrong: light ? Qt.rgba(0, 0, 0, 0.45) : Qt.rgba(1, 1, 1, 0.55)
    readonly property color subtleHover: light ? Qt.rgba(0, 0, 0, 0.037) : Qt.rgba(1, 1, 1, 0.061)
    readonly property color subtlePressed: light ? Qt.rgba(0, 0, 0, 0.024) : Qt.rgba(1, 1, 1, 0.042)
    readonly property color stroke: light ? Qt.rgba(0, 0, 0, 0.09) : Qt.rgba(1, 1, 1, 0.09) // panels and menus
    readonly property color divider: light ? Qt.rgba(0, 0, 0, 0.08) : Qt.rgba(1, 1, 1, 0.084)
    readonly property color card: light ? Qt.rgba(1, 1, 1, 0.7) : Qt.rgba(1, 1, 1, 0.051)
    readonly property color cardHover: light ? Qt.rgba(0.976, 0.976, 0.976, 0.5) : Qt.rgba(1, 1, 1, 0.083)
    readonly property color cardPressed: light ? Qt.rgba(0.976, 0.976, 0.976, 0.3) : Qt.rgba(1, 1, 1, 0.035)
    readonly property color cardStroke: light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(0, 0, 0, 0.18)
    // Menus and flyouts inside a window (no blur of their own).
    readonly property color flyout: light ? "#f9f9f9" : "#2c2c2c"
    readonly property color dialog: light ? "#fbfbfb" : "#2b2b2b"
    readonly property color dialogFooter: light ? "#f3f3f3" : "#202020"
    readonly property color dialogStroke: light ? Qt.rgba(0, 0, 0, 0.12) : Qt.rgba(0, 0, 0, 0.45)
    readonly property color smoke: Qt.rgba(0, 0, 0, 0.3) // behind a window's dialogs
    // The veil over the screen behind a system dialog (UAC): dark in light
    // mode too, like Windows.
    readonly property color veil: Effects.blurAvailable ? Qt.rgba(0, 0, 0, light ? 0.35 : 0.45) : Qt.rgba(0, 0, 0, 0.6)
    readonly property color field: light ? "#ffffff" : "#1e1e1e" // a text field while typing
    readonly property color focusRing: light ? Qt.rgba(0, 0, 0, 0.45) : Qt.rgba(1, 1, 1, 0.35)
    // Files: the files and navigation area, the selection, the scroll bars.
    readonly property color layer: light ? Qt.rgba(1, 1, 1, 0.5) : Qt.rgba(0.227, 0.227, 0.227, 0.30)
    readonly property color layerStroke: light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(0, 0, 0, 0.10)
    readonly property color selection: light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(1, 1, 1, 0.10)
    readonly property color selectionHover: light ? Qt.rgba(0, 0, 0, 0.09) : Qt.rgba(1, 1, 1, 0.13)
    readonly property color scrollTrack: light ? Qt.rgba(0, 0, 0, 0.04) : Qt.rgba(1, 1, 1, 0.05)
    readonly property color scrollThumb: light ? Qt.rgba(0, 0, 0, 0.35) : Qt.rgba(1, 1, 1, 0.35)
    readonly property color scrollThumbHover: light ? Qt.rgba(0, 0, 0, 0.55) : Qt.rgba(1, 1, 1, 0.55)

    // --- the shell's surfaces ---
    // With the compositor's blur (docs/renderer.md §8.3) panels are acrylic
    // like on Windows 11: semi-transparent over the blurred background.
    // Without it, opaque.
    readonly property bool acrylic: Effects.blurAvailable
    readonly property color desktop: "#06182d" // below the wallpaper, its darkest blue
    readonly property color taskbar: light
        ? (acrylic ? Qt.rgba(0.95, 0.95, 0.96, 0.66) : Qt.rgba(0.94, 0.94, 0.95, 0.96))
        : (acrylic ? Qt.rgba(0.11, 0.11, 0.13, 0.62) : Qt.rgba(0.10, 0.10, 0.12, 0.94))
    readonly property color surface: light
        ? (acrylic ? Qt.rgba(0.96, 0.96, 0.97, 0.74) : Qt.rgba(0.95, 0.95, 0.96, 0.98))
        : (acrylic ? Qt.rgba(0.14, 0.14, 0.16, 0.72) : Qt.rgba(0.14, 0.14, 0.16, 0.97))
    // The shell's menus, surfaces of their own that the compositor blurs.
    readonly property color popup: light
        ? (acrylic ? Qt.rgba(0.98, 0.98, 0.98, 0.82) : "#f9f9f9")
        : (acrylic ? Qt.rgba(0.17, 0.17, 0.19, 0.78) : "#2a2a2f")
    // Panels over the output (Task View, Snap Assist), and the window cards
    // on them.
    readonly property color backdrop: light
        ? (acrylic ? Qt.rgba(0.93, 0.93, 0.95, 0.55) : Qt.rgba(0.92, 0.92, 0.94, 0.96))
        : (acrylic ? Qt.rgba(0.05, 0.05, 0.07, 0.55) : Qt.rgba(0.07, 0.07, 0.09, 0.96))
    readonly property color taskCard: light ? Qt.rgba(0.98, 0.98, 0.99, 0.92) : Qt.rgba(0.15, 0.15, 0.17, 0.92)
    readonly property color taskCardHover: light ? Qt.rgba(1, 1, 1, 0.97) : Qt.rgba(0.20, 0.20, 0.23, 0.95)
    // To lighten (or darken, in light mode) what's below: hover on colored
    // tiles, zone backgrounds in a panel.
    readonly property color overlay: light ? "black" : "white"
    readonly property color footer: light ? Qt.rgba(0, 0, 0, 0.035) : Qt.rgba(0, 0, 0, 0.18)
    readonly property int taskbarHeight: 48

    // --- text ---
    readonly property color text: light ? Qt.rgba(0, 0, 0, 0.894) : "#ffffff"
    readonly property color textSecondary: light ? Qt.rgba(0, 0, 0, 0.62) : Qt.rgba(1, 1, 1, 0.786)
    readonly property color textTertiary: light ? Qt.rgba(0, 0, 0, 0.447) : Qt.rgba(1, 1, 1, 0.545)
    readonly property color critical: light ? "#c42b1c" : "#ff99a4"
    readonly property color caution: light ? "#9d5d00" : "#fce100"

    // --- accent: in dark mode the light tint with black text, in light mode
    // the dark one with white text ---
    readonly property color accent: Style.accent
    readonly property color accentFill: light ? Qt.darker(Style.accent, 1.15) : Qt.lighter(Style.accent, 1.35)
    readonly property color accentFillHover: light ? Qt.darker(Style.accent, 1.05) : Qt.lighter(Style.accent, 1.25)
    readonly property color accentText: light ? "#ffffff" : "#000000"
    readonly property color link: light ? Qt.darker(Style.accent, 1.3) : Qt.lighter(Style.accent, 1.5)
    // Icons of the theme matching the mode ("image://icon/l/<name>"; the
    // prefix reloads them when the mode changes). Always white on the accent.
    readonly property string icons: "image://icon/" + Style.iconMode
    readonly property string iconsOnAccent: Style.iconMode === "l/" ? "image://icon/w/" : icons

    // --- shapes and type ---
    readonly property int radius: 4 // controls
    readonly property int radiusCard: 6
    readonly property int radiusOverlay: 8 // menus, flyouts, dialogs, panels
    readonly property int fontCaption: 12
    readonly property int fontBody: 14
    readonly property int fontBodyLarge: 18
    readonly property int fontSubtitle: 20
    readonly property int fontTitle: 28

    // --- motion: four durations and one curve, the same as the compositor's
    // (compositor/src/motion.h), so windows and shell move the same way.
    // scripts/check-shared-values.py fails if the two copies disagree. ---
    readonly property var scale: ({ fast: 120, normal: 200, slow: 250, long: 300 })
    // Animation effects off (Settings > Accessibility > Visual effects; the
    // compositor reads the same keys): nothing moves, and what fades takes at
    // most `fast`. Every movement is multiplied by `motion` (1 or 0): a
    // panel's offset and scale, a page's slide, a layout change's duration.
    // On, every duration is Vela's times `duration` (Settings' slider; 1 is
    // Vela's).
    readonly property bool animationEffects: Style.animationEffects
    readonly property real motion: animationEffects ? 1 : 0
    readonly property real duration: animationEffects ? Style.animationDuration : 1
    readonly property int fast: Math.round(scale.fast * duration) // fades in, state fills
    readonly property int normal: animationEffects ? Math.round(scale.normal * duration) : fast // exits, popups, dialogs, page changes
    readonly property int slow: animationEffects ? Math.round(scale.slow * duration) : fast // panels and windows entering the scene
    readonly property int long: animationEffects ? Math.round(scale.long * duration) : fast // what moves across the whole window
    // cubic-bezier(0, 0, 0.2, 1): a decisive start, a soft landing. For exits
    // too, which are shorter: one that starts slowly seems to ignore the click.
    readonly property var decelerate: [0.0, 0.0, 0.2, 1.0, 1.0, 1.0]
}
