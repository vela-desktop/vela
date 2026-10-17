// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls

// Shadow: its reach for each depth, and `above` and `below` cutting it next
// to the target, where another translucent panel is (docs/visual-coherence.md
// §6.9).
Item {
    id: root
    width: 600
    height: 500

    Item { id: panel; x: 100; y: 100; width: 200; height: 150 }
    Shadow { id: flyout; target: panel }
    Shadow { id: dialog; target: panel; depth: Theme.shadow.dialog }
    Shadow { id: cut; target: panel; above: 6; below: 10 }

    TestCase {
        name: "Shadow"
        when: windowShown

        // The four strips, in their order: above, below, left, right.
        function strips(shadow) {
            return [0, 1, 2, 3].map(i => shadow.children[i])
        }

        function test_reach_of_each_depth() {
            compare(flyout.reach, 32 + 6 + 8)
            compare(dialog.reach, 48 + 16 + 8)
            compare(flyout.x, 100 - flyout.reach)
            compare(flyout.width, 200 + 2 * flyout.reach)
        }

        function test_whole_by_default() {
            const [top, bottom, left, right] = strips(flyout)
            compare(top.y, 0)
            compare(top.height, flyout.reach)
            compare(bottom.y, flyout.reach + 150)
            compare(bottom.height, flyout.reach)
            compare(left.height, 150)
            compare(right.x, flyout.reach + 200)
        }

        function test_cut_above_and_below() {
            const [top, bottom, left, right] = strips(cut)
            // Next to the target, as far as asked.
            compare(top.y + top.height, cut.reach)
            compare(top.height, 6)
            compare(bottom.y, cut.reach + 150)
            compare(bottom.height, 10)
            // The sides stay whole.
            compare(left.width, cut.reach)
            compare(right.width, cut.reach)
        }
    }
}
