// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls

// PanelMotion and Spatial under motion-variant=expressive (the
// *-expressive ctest entries): the entrance passes its rest by M3's default
// spatial curve's overshoot and settles, inside a blur region widened by
// that much; the exit doesn't bounce; with animation effects off nothing
// moves.
Item {
    id: root
    width: 600
    height: 500

    PanelMotion {
        id: motion
        x: 100
        y: 100
        width: 200
        height: 150
        offset: Qt.point(0, 48)
    }
    PanelMotion {
        id: grower
        x: 350
        y: 100
        width: 100
        height: 100
        scaleFrom: 0.9
    }
    Item { id: box; width: 10; height: 10 }
    NumberAnimation { id: plain }

    TestCase {
        name: "PanelMotionSpatial"
        when: windowShown

        property var shift

        function initTestCase() {
            shift = motion.transform[1]
        }

        function init() {
            // The motion variant, alone (*-expressive-motion) or in the
            // whole expressive theme.
            if (!Theme.spatialMotion)
                skip("with motion-variant=expressive (the *-expressive ctest entries)")
            motion.close(true)
            grower.close(true)
        }

        // The curves, durations and overshoots are M3's (tokens.md, "Web:
        // spring → cubic-bezier"), from the shared duration scale.
        function test_curves_are_m3s() {
            compare(Theme.motionVariant, "expressive")
            verify(Theme.spatialMotion)
            if (!Theme.animationEffects) {
                compare(Theme.spatialDuration("fast", Theme.normal), Theme.fast)
                compare(Theme.spatialCurve("fast"), Theme.decelerate)
                compare(Theme.spatialOvershoot("default"), 0)
                return
            }
            compare(Theme.spatialCurve("fast"), [0.42, 1.67, 0.21, 0.90, 1.0, 1.0])
            compare(Theme.spatialCurve("default"), [0.38, 1.21, 0.22, 1.00, 1.0, 1.0])
            compare(Theme.spatialCurve("slow"), [0.39, 1.29, 0.35, 0.98, 1.0, 1.0])
            compare(Theme.spatialDuration("fast", Theme.normal), Math.round(350 * Theme.duration))
            compare(Theme.spatialDuration("default", Theme.normal), Math.round(500 * Theme.duration))
            compare(Theme.spatialDuration("slow", Theme.normal), Math.round(650 * Theme.duration))
            // The highest point of each curve's y, found analytically:
            // y(s) = 3(1-s)²s·y1 + 3(1-s)s²·y2 + s³.
            fuzzyCompare(Theme.spatialOvershoot("fast"), 0.0921, 0.0005)
            fuzzyCompare(Theme.spatialOvershoot("default"), 0.0139, 0.0005)
            fuzzyCompare(Theme.spatialOvershoot("slow"), 0.0190, 0.0005)
        }

        function test_spatial_animation_takes_the_curve() {
            const animation = createTemporaryObject(spatialComponent, root, { speed: "fast" })
            compare(animation.duration, Theme.spatialDuration("fast", Theme.normal))
            compare(animation.easing.bezierCurve, Theme.spatialCurve("fast"))
            if (!Theme.animationEffects)
                return
            // It passes the end and comes back.
            box.x = 0
            animation.target = box
            animation.property = "x"
            animation.from = 0
            animation.to = 100
            animation.start()
            let highest = 0
            while (animation.running) {
                highest = Math.max(highest, box.x)
                wait(5)
            }
            compare(box.x, 100)
            verify(highest > 100.5, "highest " + highest)
            verify(highest <= 100 * (1 + Theme.spatialOvershoot("fast")) + 0.01, "highest " + highest)
        }

        function test_entrance_overshoots_inside_its_area() {
            if (!Theme.animationEffects)
                skip("with effects on")
            motion.open()
            verify(motion.moving)
            const a = motion.area
            const over = 48 * Theme.spatialOvershoot("default")
            verify(over > 0)
            fuzzyCompare(a.y, 100 - over, 0.001)
            fuzzyCompare(a.height, 150 + 48 + over, 0.001)
            let lowest = 1
            while (motion.moving) {
                compare(motion.area, a)
                const top = motion.y + shift.y
                verify(top >= a.y - 0.01 && top + motion.height <= a.y + a.height + 0.01,
                       "panel at " + top + " in " + a.y + "+" + a.height)
                lowest = Math.min(lowest, motion.away)
                wait(5)
            }
            verify(lowest < 0, "away went down to " + lowest)
            compare(motion.away, 0)
            compare(shift.y, 0)
        }

        // A panel that grows into place passes its size: the area holds it.
        function test_growing_panel_fits_its_area() {
            if (!Theme.animationEffects)
                skip("with effects on")
            grower.open()
            const a = grower.area
            const scale = grower.transform[0]
            let largest = 0
            while (grower.moving) {
                const w = grower.width * scale.xScale
                largest = Math.max(largest, scale.xScale)
                verify(w <= a.width + 0.01, w + " in " + a.width)
                wait(5)
            }
            verify(largest > 1, "largest scale " + largest)
        }

        function test_exit_does_not_bounce() {
            if (!Theme.animationEffects)
                skip("with effects on")
            motion.open()
            tryVerify(() => !motion.moving, 5000)
            motion.close()
            while (motion.closing) {
                verify(motion.away >= 0 && motion.away <= 1, "away " + motion.away)
                wait(5)
            }
            compare(motion.away, 1)
        }

        function test_nothing_moves_without_effects() {
            if (Theme.animationEffects)
                skip("with effects off")
            motion.open()
            verify(motion.moving)
            compare(motion.area, Qt.rect(100, 100, 200, 150))
            while (motion.moving) {
                compare(shift.y, 0)
                wait(8)
            }
            compare(motion.opacity, 1)
        }

        Component { id: spatialComponent; Spatial {} }
    }
}
