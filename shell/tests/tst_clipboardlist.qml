// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Clipboard history's list (ClipboardList.qml): each change to the history
// is a new list, as Clip.items gives it. The view stays where it was, the
// selection stays on its entry, and a selection in sight stays in sight.
Item {
    id: root
    width: 360
    height: 460

    function entries(ids) {
        return ids.map(id => ({ id: id, kind: "text", text: "Entry " + id, pinned: false }))
    }
    function range(n) {
        return Array.from({ length: n }, (_, i) => i + 1)
    }

    ClipboardList {
        id: list
        width: 360
        height: 420
        entries: root.entries(root.range(20))
    }
    SignalSpy { id: pasted; target: list; signalName: "pasteRequested" }
    SignalSpy { id: more; target: list; signalName: "moreRequested" }

    TestCase {
        name: "ClipboardList"
        when: windowShown

        function init() {
            list.entries = root.entries(root.range(20))
            list.current = 0
            list.positionViewAtBeginning()
            pasted.clear()
            more.clear()
        }

        function inSight(index) {
            const item = list.itemAtIndex(index)
            return item !== null && item.y >= list.contentY - 0.5
                && item.y + item.height <= list.contentY + list.height + 0.5
        }

        function test_the_history_given_at_creation_is_shown() {
            compare(list.count, 20)
        }

        // Del on the 17th entry, at the bottom of the view: the view stays,
        // and the selection is the next entry, in sight.
        function test_deleting_the_selection_keeps_the_view() {
            list.current = 16
            list.positionViewAtIndex(16, ListView.Contain)
            const y = list.contentY
            verify(y > list.originY)
            list.entries = root.entries(root.range(20).filter(id => id !== 17))
            compare(list.current, 16)
            compare(list.contentY, y)
            verify(inSight(16))
        }

        // The last entry: the selection moves up to the new last one.
        function test_deleting_the_last_entry_selects_the_one_before() {
            list.current = 19
            list.positionViewAtIndex(19, ListView.Contain)
            list.entries = root.entries(root.range(19))
            compare(list.current, 18)
            verify(inSight(18))
            verify(list.contentY <= list.originY + list.contentHeight - list.height + 0.5)
        }

        // With the mouse, far from the selection (the first entry): "…" and
        // Delete, or Pin, leave the view where it was.
        function test_a_change_out_of_sight_of_the_selection_keeps_the_view() {
            list.positionViewAtIndex(15, ListView.Contain)
            const y = list.contentY
            verify(!inSight(0))
            const pinned = root.entries([16]).map(e => Object.assign(e, { pinned: true }))
            list.entries = pinned.concat(root.entries(root.range(20).filter(id => id !== 16)))
            compare(list.current, 1)
            compare(list.contentY, y)
        }

        // Something copied while the panel is open comes first: the
        // selection stays on its entry, and in sight at the bottom of the
        // view.
        function test_the_selection_follows_its_entry() {
            list.current = 16
            list.positionViewAtIndex(16, ListView.Contain)
            list.entries = root.entries([21].concat(root.range(20)))
            compare(list.current, 17)
            compare(list.shown[list.current].id, 17)
            verify(inSight(17))
        }

        // With the mouse, at the end of the list: "…" and Delete on the last
        // entry. The view ends where the list now ends.
        function test_deleting_at_the_end_out_of_sight_of_the_selection() {
            list.positionViewAtEnd()
            verify(!inSight(0))
            list.entries = root.entries(root.range(19))
            compare(list.current, 0)
            verify(inSight(18))
            compare(list.contentY, list.originY + list.contentHeight - list.height)
        }

        function test_an_empty_history() {
            list.current = 5
            list.entries = []
            compare(list.count, 0)
            compare(list.current, 0)
        }

        function test_click_and_more() {
            mouseClick(list.itemAtIndex(1), 40, 20)
            compare(pasted.count, 1)
            compare(pasted.signalArguments[0][0], 1)
            const card = list.itemAtIndex(2)
            mouseClick(card, card.width - 22, 22)
            compare(more.count, 1)
            compare(more.signalArguments[0][0].id, 3)
            compare(pasted.count, 1)
        }
    }
}
