// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// The cursor size field (CursorSizeField.qml): − and + go to the theme's
// previous and next size, a typed size lands on the nearest and the field
// shows where, the buttons stop at the theme's ends, and the question mark
// explains it with this theme's sizes. The snapping itself is Preferences'
// (settings-preferences); this fake does the same.
Item {
    width: 600
    height: 100

    QtObject {
        id: prefs
        property int cursorSize: 24
        property var cursorSizes: [24, 36, 48]
        function nearestCursorSize(size) {
            if (cursorSizes.length === 0)
                return Math.max(8, Math.min(256, size))
            let nearest = cursorSizes[0]
            for (const candidate of cursorSizes) {
                if (Math.abs(candidate - size) < Math.abs(nearest - size))
                    nearest = candidate
            }
            return nearest
        }
        function steppedCursorSize(from, direction) {
            if (direction > 0)
                return cursorSizes.find(size => size > from) ?? cursorSizes[cursorSizes.length - 1]
            return cursorSizes.slice().reverse().find(size => size < from) ?? cursorSizes[0]
        }
    }

    CursorSizeField {
        id: field
        prefs: prefs
    }

    TestCase {
        name: "CursorSizeField"
        when: windowShown

        function part(name) {
            return findChild(field, name)
        }

        function init() {
            part("larger").forceActiveFocus() // not typing: the field follows
            prefs.cursorSizes = [24, 36, 48]
            prefs.cursorSize = 24
        }

        function test_steps_through_the_themes_sizes() {
            compare(part("size").text, "24")
            verify(!part("smaller").enabled, "nothing below the smallest")
            mouseClick(part("larger"))
            compare(prefs.cursorSize, 36)
            compare(part("size").text, "36")
            mouseClick(part("larger"))
            compare(prefs.cursorSize, 48)
            verify(!part("larger").enabled, "nothing above the largest")
            mouseClick(part("smaller"))
            compare(prefs.cursorSize, 36)
            compare(part("size").text, "36")
        }

        function test_a_typed_size_lands_on_the_nearest() {
            const box = part("size")
            box.forceActiveFocus()
            box.selectAll()
            keyClick("4")
            keyClick("4")
            keyClick(Qt.Key_Return)
            compare(prefs.cursorSize, 48)
            compare(box.text, "48")
            box.selectAll()
            keyClick("2")
            keyClick("5")
            keyClick(Qt.Key_Return)
            compare(prefs.cursorSize, 24)
            compare(box.text, "24")
        }

        function test_a_size_from_elsewhere_shows_the_one_in_use() {
            prefs.cursorSize = 30 // kcminputrc written by KDE
            compare(part("size").text, "24")
            mouseClick(part("larger"))
            compare(prefs.cursorSize, 36)
        }

        function test_the_question_mark_says_why() {
            const why = part("why")
            verify(why.explanation.indexOf("24, 36, 48") >= 0, why.explanation)
            verify(why.explanation.indexOf("125%") >= 0, why.explanation)
            // A click opens the explanation, Esc closes it, a click again too.
            const popup = findChild(field, "whyPopup")
            verify(!popup.visible)
            mouseClick(why)
            tryVerify(() => popup.visible)
            compare(findChild(popup.contentItem, "whyText").text, why.explanation)
            keyClick(Qt.Key_Escape)
            tryVerify(() => !popup.visible)
            mouseClick(why)
            tryVerify(() => popup.visible)
            popup.close()
            tryVerify(() => !popup.visible)
            prefs.cursorSizes = []
            verify(why.explanation.indexOf("8 to 256") >= 0, why.explanation)
            verify(part("larger").enabled && part("smaller").enabled)
        }
    }
}
