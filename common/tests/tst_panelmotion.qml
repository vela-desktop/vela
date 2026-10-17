// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls

// PanelMotion: entering, leaving, turning back when reopened during the exit
// without emitting closed(), no input while leaving, the blur area along the
// whole travel; with animation effects off only the opacity changes
// (docs/visual-coherence.md §6.9).
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
        Rectangle {
            id: content
            anchors.fill: parent
            color: "gray"
            MouseArea { id: area; anchors.fill: parent; hoverEnabled: true }
            TextInput { id: field; width: 100; height: 20 }
        }
    }
    PanelMotion {
        id: popup
        x: 350
        y: 100
        width: 100
        height: 100
        scaleFrom: 1.05
        instantExit: true
    }
    SignalSpy { id: closed; target: motion; signalName: "closed" }
    SignalSpy { id: popupClosed; target: popup; signalName: "closed" }

    TestCase {
        name: "PanelMotion"
        when: windowShown

        property var scale
        property var shift

        function initTestCase() {
            scale = motion.transform[0]
            shift = motion.transform[1]
        }

        function init() {
            motion.close()
            popup.close()
            tryCompare(motion, "opacity", 0, 3000)
            tryVerify(() => !motion.closing, 3000)
            closed.clear()
            popupClosed.clear()
        }

        function openFully() {
            motion.open()
            tryCompare(motion, "opacity", 1, 3000)
            tryCompare(motion, "away", 0, 3000)
        }

        function test_starts_hidden_at_its_origin() {
            compare(motion.opacity, 0)
            compare(motion.away, 1)
            verify(!motion.opened)
        }

        function test_enters_from_its_offset() {
            motion.open()
            verify(motion.opened)
            if (Theme.animationEffects) {
                wait(Theme.fast / 3)
                verify(shift.y > 0 && shift.y < 48, "on its way: " + shift.y)
                verify(motion.opacity > 0 && motion.opacity < 1, "fading in: " + motion.opacity)
            }
            tryCompare(motion, "opacity", 1, 3000)
            tryCompare(shift, "y", 0, 3000)
            compare(closed.count, 0)
        }

        function test_exit_ends_with_closed() {
            openFully()
            motion.close()
            verify(motion.closing)
            tryCompare(closed, "count", 1, 3000)
            verify(!motion.closing)
            compare(motion.opacity, 0)
        }

        function test_reopening_during_the_exit_turns_back() {
            openFully()
            motion.close()
            wait(Theme.normal / 3)
            const opacity = motion.opacity
            const y = shift.y
            verify(opacity > 0 && opacity < 1, "leaving: " + opacity)
            motion.open()
            verify(!motion.closing)
            // From where it was: no jump back to the start.
            verify(motion.opacity >= opacity - 0.01, motion.opacity + " from " + opacity)
            verify(shift.y <= y + 0.01, shift.y + " from " + y)
            tryCompare(motion, "opacity", 1, 3000)
            tryCompare(shift, "y", 0, 3000)
            wait(Theme.normal)
            compare(closed.count, 0)
        }

        function test_no_input_while_leaving() {
            openFully()
            mouseMove(content, 50, 50)
            verify(area.containsMouse)
            motion.close()
            const spy = createTemporaryObject(spyComponent, root, { target: area, signalName: "clicked" })
            mouseClick(content, 60, 60)
            compare(spy.count, 0)
            tryCompare(closed, "count", 1, 3000)
        }

        function test_no_keys_while_leaving_and_back_when_turning_back() {
            openFully()
            field.text = ""
            field.forceActiveFocus()
            keyClick(Qt.Key_A)
            compare(field.text, "a")
            motion.close()
            verify(!field.activeFocus)
            keyClick(Qt.Key_B)
            compare(field.text, "a", "a key while leaving")
            motion.open()
            verify(field.activeFocus, "the keys are back")
            keyClick(Qt.Key_C)
            compare(field.text, "ac")
        }

        function test_only_opacity_without_effects() {
            if (Theme.animationEffects)
                skip("with effects off")
            motion.open()
            for (let i = 0; i < 6; ++i) {
                compare(shift.y, 0)
                compare(scale.xScale, 1)
                wait(16)
            }
            tryCompare(motion, "opacity", 1, 3000)
            const area = motion.area
            compare(area.y, 100)
            compare(area.height, 150)
        }

        function test_area_covers_the_whole_travel_while_moving() {
            if (!Theme.animationEffects)
                skip("with effects on")
            motion.open()
            popup.open()
            verify(motion.moving)
            const a = motion.area
            compare(a.x, 100)
            compare(a.y, 100)
            compare(a.width, 200)
            compare(a.height, 150 + 48)
            const p = popup.area
            compare(p.x, 350 - 2.5)
            compare(p.width, 105)
            // The same on every frame of the entrance.
            while (motion.moving) {
                compare(motion.area, a)
                wait(16)
            }
            motion.close()
            verify(motion.moving)
            compare(motion.area, a)
        }

        function test_visible_area_is_cut_by_the_parent() {
            if (!Theme.animationEffects)
                skip("with effects on")
            motion.y = 380 // the travel goes 28 below the parent
            motion.open()
            verify(motion.moving)
            compare(motion.area.height, 150 + 48)
            compare(motion.visibleArea, Qt.rect(100, 380, 200, 120))
            tryVerify(() => !motion.moving, 3000)
            compare(motion.visibleArea, Qt.rect(100, 380, 200, 120))
            motion.y = 100
        }

        function test_area_is_the_panel_at_rest() {
            openFully()
            tryVerify(() => !motion.moving, 3000)
            compare(motion.area, Qt.rect(100, 100, 200, 150))
        }

        function test_instant_exit() {
            popup.open()
            tryCompare(popup, "opacity", 1, 3000)
            popup.close()
            compare(popup.opacity, 0)
            compare(popupClosed.count, 1)
            verify(!popup.closing)
        }

        function test_close_at_once() {
            openFully()
            motion.close(true)
            compare(motion.opacity, 0)
            compare(motion.away, 1)
            compare(closed.count, 1)
            verify(!motion.closing)
        }

        Component { id: spyComponent; SignalSpy {} }
    }
}
