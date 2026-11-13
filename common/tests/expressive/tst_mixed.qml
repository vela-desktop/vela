// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../colors.js" as Colors

// Each expressive piece on its own inside an otherwise default theme (the
// *-expressive-controls, -motion and -color ctest entries,
// VELA_TEST_THEME in common/tests/quicktest.h): what the piece changes, and
// that the others stay as they are. Under the whole theme or none, these
// skip.
Item {
    id: root
    width: 400
    height: 300

    Button { id: button; x: 20; y: 20; text: "Button" }
    PanelMotion {
        id: motion
        x: 100
        y: 100
        width: 200
        height: 100
        offset: Qt.point(0, 48)
    }
    property var shift

    TestCase {
        name: "MixedPieces"
        when: windowShown

        readonly property bool controlsAlone: Theme.expressive && !Theme.spatialMotion
        readonly property bool motionAlone: Theme.spatialMotion && !Theme.expressive
        readonly property bool colorAlone: !Theme.expressive && !Theme.spatialMotion
            && Theme.themed("color-source", "accent") === "wallpaper"

        function initTestCase() {
            root.shift = motion.transform[1]
        }
        function init() {
            motion.close(true)
        }

        // controls-variant=expressive alone: the expressive controls on the
        // shared curve, the accent the user's.
        function test_controls_alone() {
            if (!controlsAlone)
                skip("with controls-variant=expressive alone")
            compare(Theme.motionVariant, "default")
            verify(button.expressive)
            tryCompare(button.background, "radius", button.height / 2, 5000)
            verify(Colors.same(Theme.accentFill, Theme.primary))
            compare(Theme.spatialOvershoot("default"), 0)
            if (!Theme.animationEffects)
                return
            // A panel comes in without passing its rest.
            motion.open()
            let lowest = 1
            while (motion.moving) {
                lowest = Math.min(lowest, motion.away)
                wait(5)
            }
            verify(lowest >= 0, "away went down to " + lowest)
        }

        // motion-variant=expressive alone: Fluent's controls (WinUI's
        // corners and colors), moving on M3's spatial curves.
        function test_motion_alone() {
            if (!motionAlone)
                skip("with motion-variant=expressive alone")
            verify(!button.expressive)
            compare(button.background.radius, Theme.radius)
            verify(Colors.same(button.background.color, Theme.control))
            verify(!Colors.same(Theme.control, Theme.surfaceContainerHigh))
            if (!Theme.animationEffects) {
                compare(Theme.spatialOvershoot("default"), 0)
                return
            }
            verify(Theme.spatialOvershoot("default") > 0)
            motion.open()
            let lowest = 1
            while (motion.moving) {
                lowest = Math.min(lowest, motion.away)
                wait(5)
            }
            verify(lowest < 0, "the entrance passes its rest: " + lowest)
            compare(motion.away, 0)
        }

        // color-source=wallpaper alone: the accent is the wallpaper's color
        // (the cache the test theme leaves, #2e7d32), the controls Fluent's
        // in it, the tonal palette from it.
        function test_color_alone() {
            if (!colorAlone)
                skip("with color-source=wallpaper alone")
            const green = Qt.color("#2e7d32")
            verify(Colors.same(Style.wallpaperColor, green))
            verify(Colors.same(Style.accent, green))
            verify(Colors.same(Theme.accent, green))
            verify(!button.expressive)
            compare(button.background.radius, Theme.radius)
            const tonal = Style.tonalOf(green, Theme.light)
            verify(Colors.same(Theme.primary, tonal.primary))
            verify(Colors.same(Theme.surfaceContainerHigh, tonal.surfaceContainerHigh))
        }
    }
}
