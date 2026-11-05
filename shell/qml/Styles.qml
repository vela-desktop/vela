// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 Lowell137
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton

import QtQuick
import Vela.Controls

// The shell surfaces' styles (taskbar-style=, startmenu-style=,
// notifications-style= of vela.conf; themes/taskbar/, startmenu/,
// notifications/; docs/configuration.md, "Theme files"): sizes, shapes and
// layouts of the taskbar, the Start menu, quick settings and the notification
// center, like Windhawk's three stylers. Colors and radii shared with the
// apps are Theme's.
//
// Every token reads its key with Theme.themed() and falls back to what Vela
// draws without a style, so with no style chosen nothing changes. Numbers are
// clamped to what still lays out; flags are 0 or 1 in the files.
QtObject {
    id: styles

    function flag(key, fallback) {
        return Theme.themed(key, fallback ? 1 : 0) !== 0
    }
    function size(key, fallback, low, high) {
        return Math.max(low, Math.min(high, Math.round(Theme.themed(key, fallback))))
    }

    // --- the taskbar (themes/taskbar/) ---
    readonly property QtObject taskbar: QtObject {
        // Named built-in layouts; existing per-property styles remain compatible.
        readonly property string variant: Theme.themed("taskbar-variant", "default")
        // A look of its own (a style, or the theme's keys for it): the
        // compositor makes the bar glass from 4% alpha.
        readonly property bool chosen: Style.chosenStyles.indexOf("taskbar") >= 0
        // The bar's height; the panels it opens sit above it.
        readonly property int height: styles.size("taskbar-height", 48, 32, 96)
        // Floating: the bar is a pill this far from the window's edges, with
        // these corners; a margin or a radius makes it float.
        readonly property int margin: styles.size("taskbar-margin", variant === "default" ? 0 : 10, 0, 24)
        readonly property int radius: styles.size("taskbar-radius", variant === "default" ? 0 : 18, 0, 48)
        readonly property bool floating: margin > 0 || radius > 0
        // A floating bar that spans the screen keeps this gap at its sides.
        readonly property int sideMargin: styles.size("taskbar-side-margin", margin, 0, 400)
        // fit: apps and tray side by side in one centered pill (WindowGlass);
        // split-gap: the tray a pill of its own, that far; split-radius: the
        // corners where they part. edge: two pills, the tray at the screen's
        // right edge, the apps centered (1) or at the left (2); flush: the
        // apps' pill sits on the bottom edge.
        readonly property bool fit: styles.flag("taskbar-fit", variant !== "default")
        readonly property int splitGap: styles.size("taskbar-split-gap", variant === "floating-split" ? 8 : 0, 0, 48)
        readonly property int splitRadius: styles.size("taskbar-split-radius", 5, 0, 48)
        readonly property int edge: styles.size("taskbar-edge", variant === "dock" ? 1 : 0, 0, 2)
        readonly property bool flush: styles.flag("taskbar-flush", false)
        readonly property color stroke: Theme.themed("taskbar-stroke", Theme.stroke)
        // The buttons.
        readonly property int buttonWidth: styles.size("taskbar-button-width", 44, 24, 96)
        readonly property int buttonHeight: styles.size("taskbar-button-height", 40, 24, 96)
        readonly property int buttonRadius: styles.size("taskbar-button-radius", Theme.radius, 0, 48)
        readonly property int iconSize: styles.size("taskbar-icon-size", 26, 12, 64)
        readonly property int spacing: styles.size("taskbar-spacing", 4, 0, 24)
    }

    // --- the Start menu (themes/startmenu/) ---
    readonly property QtObject startMenu: QtObject {
        // A look of its own (a style, or the theme's keys for it): its blur is
        // rounded like the panel, glass from 4%.
        readonly property bool chosen: Style.chosenStyles.indexOf("startmenu") >= 0
        // The menu's size with its margins, as in the files (StartMenu.qml
        // takes the panel out of it): 660 x 732 is Vela's 640 x 700 panel.
        readonly property int width: styles.size("startmenu-width", 660, 360, 1600)
        readonly property int height: styles.size("startmenu-height", 732, 320, 2000)
        readonly property int radius: styles.size("startmenu-radius", Theme.radiusOverlay, 0, 48)
        readonly property color surface: Theme.themed("startmenu-surface", Theme.surface)
        readonly property int cellHeight: styles.size("startmenu-cell-height", 96, 64, 160)
    }

    // --- quick settings and the notification center (themes/notifications/) ---
    readonly property QtObject quickSettings: QtObject {
        readonly property bool chosen: Style.chosenStyles.indexOf("notifications") >= 0
        readonly property string variant: Theme.themed("quicksettings-variant", "default")
        readonly property int radius: styles.size("quicksettings-radius", variant === "classic" ? 0 : Theme.radiusOverlay, 0, 48)
        readonly property int tileRadius: styles.size("quicksettings-tile-radius", Theme.radius, 0, 24)
        readonly property color surface: Theme.themed("quicksettings-surface", Theme.surface)
        // WindowGlass's: borders only along the panel's top edge and the tiles'
        // bottom edge; a split tile's arrow right beside its icon; no footer
        // band (a line above the sliders, the mixer's icon by the arrow); a
        // thick slider with a wide knob.
        readonly property bool borderless: styles.flag("quicksettings-borderless", variant === "glass")
        readonly property bool inlineArrow: styles.flag("quicksettings-inline-arrow", variant === "glass")
        readonly property bool flatFooter: styles.flag("quicksettings-flat-footer", variant === "glass")
        readonly property bool thickSlider: styles.flag("quicksettings-thick-slider", variant === "glass")
    }
    readonly property QtObject notifications: QtObject {
        readonly property bool chosen: Style.chosenStyles.indexOf("notifications") >= 0
        readonly property int radius: styles.size("notifications-radius", Theme.radiusOverlay, 0, 48)
        readonly property int cardRadius: styles.size("notifications-card-radius", Theme.radius, 0, 24)
        readonly property color surface: Theme.themed("notifications-surface", Theme.surface)
        // Today in the calendar: a circle (16), or a rounded square.
        readonly property int todayRadius: styles.size("notifications-today-radius", 16, 0, 24)
        // focus=0: no focus session under the calendar.
        readonly property bool focus: styles.flag("notifications-focus", true)
    }

}
