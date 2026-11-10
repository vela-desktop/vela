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

    // The chosen theme's value for a token (theme= of vela.conf,
    // themes/<name>.conf): "dark.<key>" or "light.<key>" for the current mode,
    // else "<key>" for both, else Vela's own.
    function themed(key, fallback) {
        const t = Style.theme
        const modeKey = (light ? "light." : "dark.") + key
        return modeKey in t ? t[modeKey] : (key in t ? t[key] : fallback)
    }

    // --- the window: Mica, the same as Vela's title bar ---
    property bool windowActive: true // the app's Main.qml updates it
    readonly property color background: themed("background", windowActive ? Style.mica : Style.micaInactive)

    // The theme selects only Vela-owned, built-in control implementations.
    // Theme packages remain declarative: no arbitrary QML is loaded.
    readonly property string controlsVariant: themed("controls-variant", "default")

    // --- controls and surfaces ---
    readonly property color control: themed("control", light ? Qt.rgba(1, 1, 1, 0.7) : Qt.rgba(1, 1, 1, 0.061))
    readonly property color controlHover: themed("controlHover", light ? Qt.rgba(0.976, 0.976, 0.976, 0.5) : Qt.rgba(1, 1, 1, 0.084))
    readonly property color controlPressed: themed("controlPressed", light ? Qt.rgba(0.976, 0.976, 0.976, 0.3) : Qt.rgba(1, 1, 1, 0.033))
    readonly property color controlStroke: themed("controlStroke", light ? Qt.rgba(0, 0, 0, 0.07) : Qt.rgba(1, 1, 1, 0.07))
    readonly property color controlStrokeStrong: light ? Qt.rgba(0, 0, 0, 0.45) : Qt.rgba(1, 1, 1, 0.55)
    // Not themed: the title bar buttons' hover is the same (decoration.c,
    // docs/visual-coherence.md §6.6), and the compositor doesn't read themes.
    readonly property color subtleHover: light ? Qt.rgba(0, 0, 0, 0.037) : Qt.rgba(1, 1, 1, 0.061)
    readonly property color subtlePressed: light ? Qt.rgba(0, 0, 0, 0.024) : Qt.rgba(1, 1, 1, 0.042)
    // subtleHover at alpha 0: what a subtle control fades from ("transparent"
    // is black at alpha 0, and a fade from it would pass through gray).
    readonly property color subtleRest: light ? Qt.rgba(0, 0, 0, 0) : Qt.rgba(1, 1, 1, 0)
    readonly property color stroke: themed("stroke", light ? Qt.rgba(0, 0, 0, 0.09) : Qt.rgba(1, 1, 1, 0.09)) // panels and menus
    readonly property color divider: light ? Qt.rgba(0, 0, 0, 0.08) : Qt.rgba(1, 1, 1, 0.084)
    readonly property color card: themed("card", light ? Qt.rgba(1, 1, 1, 0.7) : Qt.rgba(1, 1, 1, 0.051))
    readonly property color cardHover: themed("cardHover", light ? Qt.rgba(0.976, 0.976, 0.976, 0.5) : Qt.rgba(1, 1, 1, 0.083))
    readonly property color cardPressed: themed("cardPressed", light ? Qt.rgba(0.976, 0.976, 0.976, 0.3) : Qt.rgba(1, 1, 1, 0.035))
    readonly property color cardStroke: themed("cardStroke", light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(0, 0, 0, 0.18))
    // Menus and flyouts inside a window (no blur of their own).
    readonly property color flyout: themed("flyout", light ? "#f9f9f9" : "#2c2c2c")
    readonly property color dialog: themed("dialog", light ? "#fbfbfb" : "#2b2b2b")
    readonly property color dialogFooter: themed("dialogFooter", light ? "#f3f3f3" : "#202020")
    readonly property color dialogStroke: light ? Qt.rgba(0, 0, 0, 0.12) : Qt.rgba(0, 0, 0, 0.45)
    readonly property color smoke: Qt.rgba(0, 0, 0, 0.3) // behind a window's dialogs
    // The veil over the screen behind a system dialog (UAC): dark in light
    // mode too, like Windows.
    readonly property color veil: Effects.blurAvailable ? Qt.rgba(0, 0, 0, light ? 0.35 : 0.45) : Qt.rgba(0, 0, 0, 0.6)
    readonly property color field: themed("field", light ? "#ffffff" : "#1e1e1e") // a text field while typing
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
    readonly property color taskbar: themed("taskbar", light
        ? (acrylic ? Qt.rgba(0.95, 0.95, 0.96, 0.66) : Qt.rgba(0.94, 0.94, 0.95, 0.96))
        : (acrylic ? Qt.rgba(0.11, 0.11, 0.13, 0.62) : Qt.rgba(0.10, 0.10, 0.12, 0.94)))
    readonly property color surface: themed("surface", light
        ? (acrylic ? Qt.rgba(0.96, 0.96, 0.97, 0.74) : Qt.rgba(0.95, 0.95, 0.96, 0.98))
        : (acrylic ? Qt.rgba(0.14, 0.14, 0.16, 0.72) : Qt.rgba(0.14, 0.14, 0.16, 0.97)))
    // The shell's menus, surfaces of their own that the compositor blurs.
    readonly property color popup: themed("popup", light
        ? (acrylic ? Qt.rgba(0.98, 0.98, 0.98, 0.82) : "#f9f9f9")
        : (acrylic ? Qt.rgba(0.17, 0.17, 0.19, 0.78) : "#2a2a2f"))
    // Panels over the output (Task View, Snap Assist), and the window cards
    // on them.
    readonly property color backdrop: themed("backdrop", light
        ? (acrylic ? Qt.rgba(0.93, 0.93, 0.95, 0.55) : Qt.rgba(0.92, 0.92, 0.94, 0.96))
        : (acrylic ? Qt.rgba(0.05, 0.05, 0.07, 0.55) : Qt.rgba(0.07, 0.07, 0.09, 0.96)))
    readonly property color taskCard: themed("taskCard", light ? Qt.rgba(0.98, 0.98, 0.99, 0.92) : Qt.rgba(0.15, 0.15, 0.17, 0.92))
    readonly property color taskCardHover: themed("taskCardHover", light ? Qt.rgba(1, 1, 1, 0.97) : Qt.rgba(0.20, 0.20, 0.23, 0.95))
    // To lighten (or darken, in light mode) what's below: hover on colored
    // tiles, zone backgrounds in a panel.
    readonly property color overlay: light ? "black" : "white"
    readonly property color footer: light ? Qt.rgba(0, 0, 0, 0.035) : Qt.rgba(0, 0, 0, 0.18)
    readonly property int taskbarHeight: 48

    // --- text ---
    readonly property color text: themed("text", light ? Qt.rgba(0, 0, 0, 0.894) : "#ffffff")
    readonly property color textSecondary: themed("textSecondary", light ? Qt.rgba(0, 0, 0, 0.62) : Qt.rgba(1, 1, 1, 0.786))
    readonly property color textTertiary: themed("textTertiary", light ? Qt.rgba(0, 0, 0, 0.447) : Qt.rgba(1, 1, 1, 0.545))
    readonly property color critical: light ? "#c42b1c" : "#ff99a4"
    readonly property color caution: light ? "#9d5d00" : "#fce100"

    // --- accent: in dark mode the light tint with black text, in light mode
    // the dark one with white text ---
    // A theme may bring its own accent, over the one chosen in Settings > Colors.
    readonly property color accent: themed("accent", Style.accent)
    readonly property color accentFill: accentFillOf(accent, light)
    // An accent as a fill in a mode (Settings' previews show other accents).
    function accentFillOf(color, isLight) {
        return isLight ? Qt.darker(color, 1.15) : Qt.lighter(color, 1.35)
    }
    readonly property color accentFillHover: light ? Qt.darker(accent, 1.05) : Qt.lighter(accent, 1.25)
    // Windows' text on the accent, unless the accent is too light or too dark
    // for it (a custom color, Settings > Colors): then the other one.
    readonly property color accentText: readableOn(accentFill, light ? "#ffffff" : "#000000")
    // Text over a color: `preferred` (white or black) if it reads there
    // (contrast at least 3:1, WCAG's for large text and controls), else the
    // other one.
    function readableOn(background, preferred) {
        const text = luminanceOf(preferred)
        return contrastOf(background, preferred) >= 3 ? asColor(preferred) : Qt.color(text > 0.5 ? "#000000" : "#ffffff")
    }
    // A "#rrggbb" string or a color as a color: Qt.color() takes only
    // strings.
    function asColor(c) {
        return typeof c === "string" ? Qt.color(c) : c
    }
    // WCAG's relative luminance and contrast ratio (1 to 21).
    function luminanceOf(c) {
        const color = asColor(c)
        const channel = v => v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
        return 0.2126 * channel(color.r) + 0.7152 * channel(color.g) + 0.0722 * channel(color.b)
    }
    function contrastOf(a, b) {
        const la = luminanceOf(a)
        const lb = luminanceOf(b)
        return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
    }
    // A color part `t` (0 to 1) of the way from `a` to `b`, opaque.
    function mix(a, b, t) {
        const x = asColor(a)
        const y = asColor(b)
        return Qt.rgba(x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, 1)
    }
    // Content over a tonal role: the theme's color (or Vela's) if it reads
    // there at 4.5:1 (WCAG's for body text), else black or white, whichever
    // reads better. A theme can tint it, never make it unreadable.
    function legibleOn(background, preferred) {
        if (contrastOf(background, preferred) >= 4.5)
            return asColor(preferred)
        return Qt.color(contrastOf(background, "#000000") >= contrastOf(background, "#ffffff") ? "#000000" : "#ffffff")
    }
    // Semantic tonal roles for optional expressive controls. These are
    // static fallbacks, not a wallpaper-derived Material HCT palette: a theme
    // that sets only `primary` (or none) gets containers tinted by the accent,
    // never a fixed hue that clashes with it.
    readonly property color primary: themed("primary", accentFill)
    // The theme file's on… keys; QML reads a property named on<Name> as a
    // signal handler, so here they are <role>Text, like accentText.
    readonly property color primaryText: legibleOn(primary, themed("onPrimary", light ? "#ffffff" : "#000000"))
    readonly property color primaryContainer: themed("primaryContainer", light ? mix(primary, "#ffffff", 0.75) : mix(primary, "#000000", 0.6))
    readonly property color primaryContainerText: legibleOn(primaryContainer, themed("onPrimaryContainer", light ? mix(primary, "#000000", 0.6) : mix(primary, "#ffffff", 0.75)))
    readonly property color secondaryContainer: themed("secondaryContainer", light ? mix(primary, "#f0f0f0", 0.82) : mix(primary, "#2c2c2c", 0.8))
    readonly property color secondaryContainerText: legibleOn(secondaryContainer, themed("onSecondaryContainer", light ? mix(primary, "#000000", 0.8) : mix(primary, "#ffffff", 0.85)))
    readonly property color surfaceContainer: themed("surfaceContainer", light ? mix(primary, "#f5f5f5", 0.94) : mix(primary, "#1f1f1f", 0.93))
    readonly property color surfaceContainerHigh: themed("surfaceContainerHigh", light ? mix(primary, "#ebebeb", 0.92) : mix(primary, "#2a2a2a", 0.91))
    readonly property color outline: themed("outline", light ? mix(primary, "#737373", 0.85) : mix(primary, "#8f8f8f", 0.85))
    readonly property color link: light ? Qt.darker(accent, 1.3) : Qt.lighter(accent, 1.5)
    // Icons of the theme matching the mode ("image://icon/l/<name>"; the
    // prefix reloads them when the mode changes). Always white on the accent.
    readonly property string icons: "image://icon/" + Style.iconMode
    readonly property string iconsOnAccent: Style.iconMode === "l/" ? "image://icon/w/" : icons

    // --- shapes and type ---
    // A theme's radii are clamped to 0-24 px: beyond that controls stop being shapes.
    readonly property int radius: Math.max(0, Math.min(24, themed("radius", 4))) // controls
    readonly property int radiusCard: Math.max(0, Math.min(24, themed("radiusCard", 6)))
    readonly property int radiusOverlay: Math.max(0, Math.min(24, themed("radiusOverlay", 8))) // menus, flyouts, dialogs, panels
    // Additional semantic shapes for expressive layouts.
    readonly property int shapeSmall: Math.max(0, Math.min(24, themed("shapeSmall", 8)))
    readonly property int shapeMedium: Math.max(0, Math.min(32, themed("shapeMedium", 12)))
    readonly property int shapeLarge: Math.max(0, Math.min(48, themed("shapeLarge", 24)))
    // Shadows (Shadow.qml): WinUI's elevations for what floats. Flyouts:
    // menus, panels, previews; dialogs: above a window or the screen. The
    // compositor gives windows their own (scene/, two layers).
    readonly property var shadow: ({
        flyout: { blur: 32, offset: 6, opacity: 0.45 },
        dialog: { blur: 48, offset: 16, opacity: 0.45 }
    })
    readonly property int fontCaption: Math.max(9, Math.min(24, themed("fontCaption", 12)))
    readonly property int fontBody: Math.max(10, Math.min(28, themed("fontBody", 14)))
    readonly property int fontBodyLarge: Math.max(12, Math.min(36, themed("fontBodyLarge", 18)))
    readonly property int fontSubtitle: Math.max(14, Math.min(44, themed("fontSubtitle", 20)))
    readonly property int fontTitle: Math.max(16, Math.min(64, themed("fontTitle", 28)))

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
