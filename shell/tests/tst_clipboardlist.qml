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
    // Short texts first, then images: an image's card is more than twice
    // the height of a one-line text's.
    function mixed(ids) {
        return ids.map(id => id <= 6
            ? { id: id, kind: "text", text: "Entry " + id, pinned: false }
            : { id: id, kind: "image", text: "", pinned: false })
    }

    ClipboardList {
        id: list
        width: 360
        height: 420
        entries: root.entries(root.range(20))
    }
    // The panel's own Up and Down (ClipboardPanel.qml): the keys the list
    // lets through.
    property var keysReached: []
    Keys.onPressed: event => root.keysReached.push(event.key)
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
            root.keysReached = []
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

        // With the mouse, far from the selection (the first entry): Pin on
        // the last entry in sight takes it to the top, and the others stay
        // where they were in the view.
        function test_a_change_out_of_sight_of_the_selection_keeps_the_view() {
            list.positionViewAtIndex(15, ListView.Contain)
            verify(!inSight(0))
            const first = firstInSight()
            const before = offsetOf(first)
            const pinned = root.entries([16]).map(e => Object.assign(e, { pinned: true }))
            list.entries = pinned.concat(root.entries(root.range(20).filter(id => id !== 16)))
            compare(list.current, 1)
            compare(offsetOf(first), before)
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

        // The first entry in sight, at the top of the view or below it.
        function firstInSight() {
            for (let i = 0; i < list.count; ++i) {
                const item = list.itemAtIndex(i)
                if (item !== null && item.y + item.height > list.contentY) return list.shown[i].id
            }
            return -1
        }

        // Where an entry is in the view: its distance from the view's top.
        function offsetOf(id) {
            const index = list.shown.findIndex(e => e.id === id)
            const item = list.itemAtIndex(index)
            verify(item !== null, "entry " + id + " is laid out")
            return item.y - list.contentY
        }

        // Among the images, far from the selection (the first entry), with
        // texts at the top: something new is copied, out of sight. The
        // entries in sight stay where they were in the view.
        function test_a_new_copy_keeps_the_images_in_sight() {
            list.entries = root.mixed(root.range(30))
            list.current = 0
            list.positionViewAtIndex(26, ListView.Contain)
            verify(!inSight(0))
            const before = offsetOf(26)
            list.entries = root.mixed([31].concat(root.range(30)))
            compare(list.current, 1)
            compare(offsetOf(26), before)
        }

        // The same with "…" and Delete on an entry out of sight above.
        function test_deleting_above_keeps_the_images_in_sight() {
            list.entries = root.mixed(root.range(30))
            list.current = 0
            list.positionViewAtIndex(26, ListView.Contain)
            const before = offsetOf(26)
            list.entries = root.mixed(root.range(30).filter(id => id !== 3))
            compare(offsetOf(26), before)
        }

        // Pin on the first entry in sight: it goes to the top of the list,
        // and the entries after it stay where they were.
        function test_pinning_the_first_entry_in_sight_keeps_the_others() {
            list.entries = root.mixed(root.range(30))
            list.current = 0
            list.positionViewAtIndex(26, ListView.Contain)
            const first = firstInSight()
            const next = first + 1
            const before = offsetOf(next)
            const entries = root.mixed(root.range(30))
            const pinned = entries.splice(first - 1, 1).map(e => Object.assign(e, { pinned: true }))
            list.entries = pinned.concat(entries)
            compare(offsetOf(next), before)
        }

        // "…" and Pin on the selection, in sight among the images: it goes to
        // the top of the list, and the view follows it there.
        function test_pinning_the_selection_keeps_it_in_sight() {
            list.entries = root.mixed(root.range(30))
            list.current = 25
            list.positionViewAtIndex(26, ListView.Contain)
            verify(inSight(25))
            const entries = root.mixed(root.range(30))
            const pinned = entries.splice(25, 1).map(e => Object.assign(e, { pinned: true }))
            list.entries = pinned.concat(entries)
            compare(list.current, 0)
            verify(inSight(0))
        }

        // A new copy while the selection is the top card, cut off by the
        // view's top: it stays as it was, the view doesn't move to show it
        // whole.
        function test_a_selection_cut_off_at_the_top_stays_put() {
            list.current = 5
            list.positionViewAtIndex(5, ListView.Beginning)
            list.contentY += 20
            const before = offsetOf(6)
            compare(before, -20)
            list.entries = root.entries([21].concat(root.range(20)))
            compare(list.current, 6)
            compare(offsetOf(6), before)
        }

        // The same at the bottom of the view.
        function test_a_selection_cut_off_at_the_bottom_stays_put() {
            list.current = 10
            list.positionViewAtIndex(10, ListView.End)
            list.contentY -= 20
            const before = offsetOf(11)
            list.entries = root.entries([21].concat(root.range(20)))
            compare(list.current, 11)
            compare(offsetOf(11), before)
        }

        // Tab to a card's "…", then Down: the panel moves the selection, not
        // the list on its own.
        function test_arrows_on_a_card_button_reach_the_panel() {
            const button = moreButton(list.itemAtIndex(0))
            button.forceActiveFocus(Qt.TabFocusReason)
            verify(button.activeFocus)
            keyClick(Qt.Key_Down)
            keyClick(Qt.Key_Up)
            compare(root.keysReached, [Qt.Key_Down, Qt.Key_Up])
        }

        function moreButton(card) {
            for (const child of card.children) {
                if (child.text === "…") return child
            }
            return null
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
