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

    // The theme selects only Vela-owned, built-in control implementations.
    // Theme packages remain declarative: no arbitrary QML is loaded.
    readonly property string controlsVariant: themed("controls-variant", "default")
    // The expressive variant (Material 3 Expressive's look): the Fluent
    // tokens below take Material's tonal roles instead (the roles at the end
    // of the colors), so every surface follows the palette without knowing
    // it. A theme's own value for a token still wins.
    readonly property bool expressive: controlsVariant === "expressive"
    // Where the palette starts (accentsource.h): "accent", or "wallpaper"
    // for the wallpaper's source color. Style resolves it; listed here so
    // that theme files may set it.
    readonly property string colorSource: themed("color-source", "accent")

    // Material's state layers: the content's color over its container, 8%
    // hovered, 10% pressed (M3's interaction states).
    readonly property real stateHover: 0.08
    readonly property real statePressed: 0.10

    // --- the window: Mica, the same as Vela's title bar; expressive, the
    // opaque surface (the title bar too: main.cpp's sendLook) ---
    property bool windowActive: true // the app's Main.qml updates it
    readonly property color background: themed("background", expressive ? surfaceTone : windowActive ? Style.mica : Style.micaInactive)

    // --- controls and surfaces ---
    // Expressive: controls on surface container high, with state layers of
    // the surface's text (Vela's mapping, as the others below).
    readonly property color control: themed("control", expressive ? surfaceContainerHigh : light ? Qt.rgba(1, 1, 1, 0.7) : Qt.rgba(1, 1, 1, 0.061))
    readonly property color controlHover: themed("controlHover", expressive ? mix(surfaceContainerHigh, surfaceText, stateHover) : light ? Qt.rgba(0.976, 0.976, 0.976, 0.5) : Qt.rgba(1, 1, 1, 0.084))
    readonly property color controlPressed: themed("controlPressed", expressive ? mix(surfaceContainerHigh, surfaceText, statePressed) : light ? Qt.rgba(0.976, 0.976, 0.976, 0.3) : Qt.rgba(1, 1, 1, 0.033))
    readonly property color controlStroke: themed("controlStroke", expressive ? outlineVariant : light ? Qt.rgba(0, 0, 0, 0.07) : Qt.rgba(1, 1, 1, 0.07))
    // A target's boundary: outline (M3: 3:1 with the surface).
    readonly property color controlStrokeStrong: expressive ? outline : light ? Qt.rgba(0, 0, 0, 0.45) : Qt.rgba(1, 1, 1, 0.55)
    // Not themed: the title bar buttons' hover is the same (decoration.c,
    // docs/visual-coherence.md §6.6), and the compositor doesn't read themes.
    readonly property color subtleHover: light ? Qt.rgba(0, 0, 0, 0.037) : Qt.rgba(1, 1, 1, 0.061)
    readonly property color subtlePressed: light ? Qt.rgba(0, 0, 0, 0.024) : Qt.rgba(1, 1, 1, 0.042)
    // subtleHover at alpha 0: what a subtle control fades from ("transparent"
    // is black at alpha 0, and a fade from it would pass through gray).
    readonly property color subtleRest: light ? Qt.rgba(0, 0, 0, 0) : Qt.rgba(1, 1, 1, 0)
    // What a subtle control (a row, a crumb, a tool) shows at rest, hovered
    // and pressed: the subtle colors above, expressive Material's state
    // layers of on surface (8%, 10%, from its alpha 0 at rest).
    readonly property color hoverLayer: expressive ? Qt.rgba(surfaceText.r, surfaceText.g, surfaceText.b, stateHover) : subtleHover
    readonly property color pressedLayer: expressive ? Qt.rgba(surfaceText.r, surfaceText.g, surfaceText.b, statePressed) : subtlePressed
    readonly property color restLayer: expressive ? Qt.rgba(surfaceText.r, surfaceText.g, surfaceText.b, 0) : subtleRest
    // Expressive: dividers and multi-element outlines are outline variant
    // (M3), cards surface container low.
    readonly property color stroke: themed("stroke", expressive ? outlineVariant : light ? Qt.rgba(0, 0, 0, 0.09) : Qt.rgba(1, 1, 1, 0.09)) // panels and menus
    readonly property color divider: expressive ? outlineVariant : light ? Qt.rgba(0, 0, 0, 0.08) : Qt.rgba(1, 1, 1, 0.084)
    readonly property color card: themed("card", expressive ? surfaceContainerLow : light ? Qt.rgba(1, 1, 1, 0.7) : Qt.rgba(1, 1, 1, 0.051))
    readonly property color cardHover: themed("cardHover", expressive ? mix(surfaceContainerLow, surfaceText, stateHover) : light ? Qt.rgba(0.976, 0.976, 0.976, 0.5) : Qt.rgba(1, 1, 1, 0.083))
    readonly property color cardPressed: themed("cardPressed", expressive ? mix(surfaceContainerLow, surfaceText, statePressed) : light ? Qt.rgba(0.976, 0.976, 0.976, 0.3) : Qt.rgba(1, 1, 1, 0.035))
    readonly property color cardStroke: themed("cardStroke", expressive ? outlineVariant : light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(0, 0, 0, 0.18))
    // Menus and flyouts inside a window (no blur of their own). Expressive:
    // surface container high, the footer in the dialog's color.
    readonly property color flyout: themed("flyout", expressive ? surfaceContainerHigh : light ? "#f9f9f9" : "#2c2c2c")
    readonly property color dialog: themed("dialog", expressive ? surfaceContainerHigh : light ? "#fbfbfb" : "#2b2b2b")
    readonly property color dialogFooter: themed("dialogFooter", expressive ? dialog : light ? "#f3f3f3" : "#202020")
    readonly property color dialogStroke: light ? Qt.rgba(0, 0, 0, 0.12) : Qt.rgba(0, 0, 0, 0.45)
    readonly property color smoke: Qt.rgba(0, 0, 0, 0.3) // behind a window's dialogs
    // The veil over the screen behind a system dialog (UAC): dark in light
    // mode too, like Windows.
    readonly property color veil: Effects.blurAvailable ? Qt.rgba(0, 0, 0, light ? 0.35 : 0.45) : Qt.rgba(0, 0, 0, 0.6)
    // Expressive: a filled field on surface container highest; the ring in
    // outline, 3:1 with the surfaces.
    readonly property color field: themed("field", expressive ? surfaceContainerHighest : light ? "#ffffff" : "#1e1e1e") // a text field while typing
    readonly property color focusRing: expressive ? outline : light ? Qt.rgba(0, 0, 0, 0.45) : Qt.rgba(1, 1, 1, 0.35)
    // Files: the files and navigation area, the selection, the scroll bars.
    // Expressive: the selection in secondary container.
    // Expressive: the files area on surface container low, as the cards
    // (Vela's choice: the content one step from the window's surface).
    readonly property color layer: expressive ? surfaceContainerLow : light ? Qt.rgba(1, 1, 1, 0.5) : Qt.rgba(0.227, 0.227, 0.227, 0.30)
    readonly property color layerStroke: expressive ? outlineVariant : light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(0, 0, 0, 0.10)
    readonly property color selection: expressive ? secondaryContainer : light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(1, 1, 1, 0.10)
    readonly property color selectionHover: expressive ? mix(secondaryContainer, secondaryContainerText, stateHover) : light ? Qt.rgba(0, 0, 0, 0.09) : Qt.rgba(1, 1, 1, 0.13)
    readonly property color scrollTrack: light ? Qt.rgba(0, 0, 0, 0.04) : Qt.rgba(1, 1, 1, 0.05)
    readonly property color scrollThumb: light ? Qt.rgba(0, 0, 0, 0.35) : Qt.rgba(1, 1, 1, 0.35)
    readonly property color scrollThumbHover: light ? Qt.rgba(0, 0, 0, 0.55) : Qt.rgba(1, 1, 1, 0.55)

    // --- the shell's surfaces ---
    // With the compositor's blur (docs/renderer.md §8.3) panels are acrylic
    // like on Windows 11: semi-transparent over the blurred background.
    // Without it, opaque. Expressive: opaque surface container even with
    // the blur, the panels' tone being their hierarchy (M3: navigation is
    // surface container), the shell's menus surface container high.
    readonly property bool acrylic: Effects.blurAvailable
    readonly property color desktop: "#06182d" // below the wallpaper, its darkest blue
    readonly property color taskbar: themed("taskbar", expressive ? surfaceContainer : light
        ? (acrylic ? Qt.rgba(0.95, 0.95, 0.96, 0.66) : Qt.rgba(0.94, 0.94, 0.95, 0.96))
        : (acrylic ? Qt.rgba(0.11, 0.11, 0.13, 0.62) : Qt.rgba(0.10, 0.10, 0.12, 0.94)))
    readonly property color surface: themed("surface", expressive ? surfaceContainer : light
        ? (acrylic ? Qt.rgba(0.96, 0.96, 0.97, 0.74) : Qt.rgba(0.95, 0.95, 0.96, 0.98))
        : (acrylic ? Qt.rgba(0.14, 0.14, 0.16, 0.72) : Qt.rgba(0.14, 0.14, 0.16, 0.97)))
    // The shell's menus, surfaces of their own that the compositor blurs.
    readonly property color popup: themed("popup", expressive ? surfaceContainerHigh : light
        ? (acrylic ? Qt.rgba(0.98, 0.98, 0.98, 0.82) : "#f9f9f9")
        : (acrylic ? Qt.rgba(0.17, 0.17, 0.19, 0.78) : "#2a2a2f"))
    // Panels over the output (Task View, Snap Assist), and the window cards
    // on them.
    readonly property color backdrop: themed("backdrop", expressive ? surfaceContainer : light
        ? (acrylic ? Qt.rgba(0.93, 0.93, 0.95, 0.55) : Qt.rgba(0.92, 0.92, 0.94, 0.96))
        : (acrylic ? Qt.rgba(0.05, 0.05, 0.07, 0.55) : Qt.rgba(0.07, 0.07, 0.09, 0.96)))
    readonly property color taskCard: themed("taskCard", expressive ? surfaceContainer : light ? Qt.rgba(0.98, 0.98, 0.99, 0.92) : Qt.rgba(0.15, 0.15, 0.17, 0.92))
    readonly property color taskCardHover: themed("taskCardHover", expressive ? mix(surfaceContainer, surfaceText, stateHover) : light ? Qt.rgba(1, 1, 1, 0.97) : Qt.rgba(0.20, 0.20, 0.23, 0.95))
    // To lighten (or darken, in light mode) what's below: hover on colored
    // tiles, zone backgrounds in a panel.
    readonly property color overlay: light ? "black" : "white"
    readonly property color footer: light ? Qt.rgba(0, 0, 0, 0.035) : Qt.rgba(0, 0, 0, 0.18)
    readonly property int taskbarHeight: 48

    // --- text: expressive, on surface, on surface variant and outline ---
    readonly property color text: themed("text", expressive ? surfaceText : light ? Qt.rgba(0, 0, 0, 0.894) : "#ffffff")
    readonly property color textSecondary: themed("textSecondary", expressive ? surfaceVariantText : light ? Qt.rgba(0, 0, 0, 0.62) : Qt.rgba(1, 1, 1, 0.786))
    readonly property color textTertiary: themed("textTertiary", expressive ? outline : light ? Qt.rgba(0, 0, 0, 0.447) : Qt.rgba(1, 1, 1, 0.545))
    readonly property color critical: expressive ? error : light ? "#c42b1c" : "#ff99a4"
    readonly property color caution: light ? "#9d5d00" : "#fce100"

    // --- accent: in dark mode the light tint with black text, in light mode
    // the dark one with white text ---
    // A theme may bring its own accent, over the one chosen in Settings > Colors.
    readonly property color accent: themed("accent", Style.accent)
    // Expressive: the accent fills are primary, with its text and state
    // layer.
    readonly property color accentFill: expressive ? primary : accentFillOf(accent, light)
    // An accent as a fill in a mode (Settings' previews show other accents).
    function accentFillOf(color, isLight) {
        return isLight ? Qt.darker(color, 1.15) : Qt.lighter(color, 1.35)
    }
    readonly property color accentFillHover: expressive ? mix(primary, primaryText, stateHover) : light ? Qt.darker(accent, 1.05) : Qt.lighter(accent, 1.25)
    // Windows' text on the accent, unless the accent is too light or too dark
    // for it (a custom color, Settings > Colors): then the other one.
    readonly property color accentText: expressive ? primaryText : readableOn(accentFill, light ? "#ffffff" : "#000000")
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
    // Material's color roles: Style.tonal, the Tonal Spot palette of the
    // theme's primary or of the effective accent (the wallpaper's, when
    // asked: accentsource.h), each one themable. A theme that sets only
    // `primary` gets containers of the same hue.
    // The theme file's on… keys; QML reads a property named on<Name> as a
    // signal handler, so here they are <role>Text, like accentText. Their
    // content reads on the role at 4.5:1 whatever the theme sets
    // (legibleOn()).
    readonly property color primary: themed("primary", Style.tonal.primary)
    readonly property color primaryText: legibleOn(primary, themed("onPrimary", Style.tonal.onPrimary))
    readonly property color primaryContainer: themed("primaryContainer", Style.tonal.primaryContainer)
    readonly property color primaryContainerText: legibleOn(primaryContainer, themed("onPrimaryContainer", Style.tonal.onPrimaryContainer))
    readonly property color secondary: themed("secondary", Style.tonal.secondary)
    readonly property color secondaryText: legibleOn(secondary, themed("onSecondary", Style.tonal.onSecondary))
    readonly property color secondaryContainer: themed("secondaryContainer", Style.tonal.secondaryContainer)
    readonly property color secondaryContainerText: legibleOn(secondaryContainer, themed("onSecondaryContainer", Style.tonal.onSecondaryContainer))
    readonly property color tertiary: themed("tertiary", Style.tonal.tertiary)
    readonly property color tertiaryText: legibleOn(tertiary, themed("onTertiary", Style.tonal.onTertiary))
    readonly property color tertiaryContainer: themed("tertiaryContainer", Style.tonal.tertiaryContainer)
    readonly property color tertiaryContainerText: legibleOn(tertiaryContainer, themed("onTertiaryContainer", Style.tonal.onTertiaryContainer))
    readonly property color error: themed("error", Style.tonal.error)
    readonly property color errorText: legibleOn(error, themed("onError", Style.tonal.onError))
    readonly property color errorContainer: themed("errorContainer", Style.tonal.errorContainer)
    readonly property color errorContainerText: legibleOn(errorContainer, themed("onErrorContainer", Style.tonal.onErrorContainer))
    // Material's surface (the body's color); `surface` above is the shell's
    // panels, so this one is surfaceTone, and surfaceBase in theme files.
    readonly property color surfaceTone: themed("surfaceBase", Style.tonal.surface)
    readonly property color surfaceContainerLowest: themed("surfaceContainerLowest", Style.tonal.surfaceContainerLowest)
    readonly property color surfaceContainerLow: themed("surfaceContainerLow", Style.tonal.surfaceContainerLow)
    readonly property color surfaceContainer: themed("surfaceContainer", Style.tonal.surfaceContainer)
    readonly property color surfaceContainerHigh: themed("surfaceContainerHigh", Style.tonal.surfaceContainerHigh)
    readonly property color surfaceContainerHighest: themed("surfaceContainerHighest", Style.tonal.surfaceContainerHighest)
    // On surface and on surface variant: on every surface container.
    readonly property color surfaceText: legibleOn(surfaceTone, themed("onSurface", Style.tonal.onSurface))
    readonly property color surfaceVariantText: legibleOn(surfaceTone, themed("onSurfaceVariant", Style.tonal.onSurfaceVariant))
    readonly property color outline: themed("outline", Style.tonal.outline)
    readonly property color outlineVariant: themed("outlineVariant", Style.tonal.outlineVariant)
    readonly property color inverseSurface: themed("inverseSurface", Style.tonal.inverseSurface)
    readonly property color inverseSurfaceText: legibleOn(inverseSurface, themed("inverseOnSurface", Style.tonal.inverseOnSurface))
    // Actions on the inverse surface (M3's snackbar action).
    readonly property color inversePrimary: themed("inversePrimary", Style.tonal.inversePrimary)
    // Expressive: links are primary (M3, underlined where they're used).
    readonly property color link: expressive ? primary : light ? Qt.darker(accent, 1.3) : Qt.lighter(accent, 1.5)
    // Icons of the theme matching the mode ("image://icon/l/<name>"; the
    // prefix reloads them when the mode changes). Always white on the accent.
    readonly property string icons: "image://icon/" + Style.iconMode
    readonly property string iconsOnAccent: Style.iconMode === "l/" ? "image://icon/w/" : icons

    // --- shapes and type ---
    // A theme's radii are clamped (controls to 0-24 px, cards and overlays
    // to 0-32, M3's extra large increased): beyond that they stop being
    // shapes.
    readonly property int radius: Math.max(0, Math.min(24, themed("radius", 4))) // controls
    readonly property int radiusCard: Math.max(0, Math.min(32, themed("radiusCard", 6)))
    readonly property int radiusOverlay: Math.max(0, Math.min(32, themed("radiusOverlay", 8))) // menus, flyouts, dialogs, panels
    // Additional semantic shapes for expressive layouts: steps of M3's
    // corner scale (extra small 4, small 8, medium 12, extra large 28), up
    // to its extra extra large, 48. shapeLarge is Vela's 24 (M3's large is
    // 16), what the expressive foundation's surfaces were drawn with.
    readonly property int shapeExtraSmall: Math.max(0, Math.min(48, themed("shapeExtraSmall", 4)))
    readonly property int shapeSmall: Math.max(0, Math.min(24, themed("shapeSmall", 8)))
    readonly property int shapeMedium: Math.max(0, Math.min(32, themed("shapeMedium", 12)))
    readonly property int shapeLarge: Math.max(0, Math.min(48, themed("shapeLarge", 24)))
    readonly property int shapeExtraLarge: Math.max(0, Math.min(48, themed("shapeExtraLarge", 28)))
    // Shadows (Shadow.qml): WinUI's elevations for what floats. Flyouts:
    // menus, panels, previews; dialogs: above a window or the screen. The
    // compositor gives windows their own (scene/, two layers).
    // Expressive: lower, as M3's hierarchy is tonal: its menus' level 2
    // (3 dp) and dialogs' level 3 (6 dp) as the offset; the blur (four
    // times the offset) and the opacity are Vela's choice, M3 publishes
    // only the levels.
    // (A block: a conditional between two object literals came out
    // undefined from the compiled binding.)
    readonly property var shadow: {
        if (expressive) {
            return {
                flyout: { blur: 12, offset: 3, opacity: 0.3 },
                dialog: { blur: 24, offset: 6, opacity: 0.3 }
            }
        }
        return {
            flyout: { blur: 32, offset: 6, opacity: 0.45 },
            dialog: { blur: 48, offset: 16, opacity: 0.45 }
        }
    }
    readonly property int fontCaption: Math.max(9, Math.min(24, themed("fontCaption", 12)))
    readonly property int fontBody: Math.max(10, Math.min(28, themed("fontBody", 14)))
    readonly property int fontBodyLarge: Math.max(12, Math.min(36, themed("fontBodyLarge", 18)))
    readonly property int fontSubtitle: Math.max(14, Math.min(44, themed("fontSubtitle", 20)))
    readonly property int fontTitle: Math.max(16, Math.min(64, themed("fontTitle", 28)))
    // The system font, only heavier where M3 Expressive asks: labels Medium
    // (M3's label styles), emphasis Bold (its emphasized styles). Not theme
    // keys: the weight follows the variant.
    readonly property int weightLabel: expressive ? Font.Medium : Font.Normal
    readonly property int weightEmphasis: expressive ? Font.Bold : Font.DemiBold

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

    // --- spatial motion: QML only, the compositor keeps the curve above
    // (windows don't bounce). motion-variant=expressive moves positions,
    // sizes and corners on M3 Expressive's spatial springs, as the
    // cubic-bezier curves and durations M3 publishes for springs without a
    // gesture (its web conversion). Colors and opacity keep ColorFade and
    // Eased: they never overshoot (M3's effects springs). ---
    readonly property string motionVariant: themed("motion-variant", "default")
    readonly property bool spatialMotion: motionVariant === "expressive"
    // M3's milliseconds (`ms`), scaled by spatialDuration() like every
    // duration.
    readonly property var spatial: ({
        fast: { curve: [0.42, 1.67, 0.21, 0.90, 1.0, 1.0], ms: 350 },
        "default": { curve: [0.38, 1.21, 0.22, 1.00, 1.0, 1.0], ms: 500 },
        slow: { curve: [0.39, 1.29, 0.35, 0.98, 1.0, 1.0], ms: 650 }
    })
    // Vela's rule (M3 prescribes none): fast for small controls (a switch's
    // knob, a button's corners, a tile), default for panels; slow is unused
    // in the shell. Exits don't bounce: they stay Eased, in `normal`.
    // The curve of a speed ("fast", "default", "slow"): decelerate with the
    // default variant or without animation effects.
    function spatialCurve(speed) {
        return spatialMotion && animationEffects ? (spatial[speed] || spatial["default"]).curve : decelerate
    }
    // Its duration: `fallback` (the default variant's, a Theme token) with
    // the default variant, `fast` without animation effects, else M3's times
    // the duration scale.
    function spatialDuration(speed, fallback) {
        if (!spatialMotion)
            return fallback
        if (!animationEffects)
            return fast
        return Math.round((spatial[speed] || spatial["default"]).ms * duration)
    }
    // How far past its end the curve of a speed goes, as a fraction of the
    // distance (0 when it doesn't): what a panel's blur region must leave
    // room for. Computed once per curve.
    readonly property var spatialOvershoots: ({
        fast: overshootOf(spatial.fast.curve),
        "default": overshootOf(spatial["default"].curve),
        slow: overshootOf(spatial.slow.curve)
    })
    function spatialOvershoot(speed) {
        return spatialMotion && animationEffects ? spatialOvershoots[speed] || spatialOvershoots["default"] : 0
    }
    // The highest point of a cubic Bézier's y (from 0 to 1, control points
    // y1 and y2), above 1.
    function overshootOf(curve) {
        let top = 1
        for (let i = 1; i < 1000; ++i) {
            const s = i / 1000
            const y = 3 * (1 - s) * (1 - s) * s * curve[1] + 3 * (1 - s) * s * s * curve[3] + s * s * s
            top = Math.max(top, y)
        }
        return top - 1
    }
}
