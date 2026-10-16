// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import "../qml/tiles.js" as Tiles

// Start's keyboard selection (tiles.js): round a section left and right, by
// column up and down, from the pinned apps to all apps and round.
TestCase {
    name: "Tiles"

    function go(counts, section, index, direction) {
        const to = Tiles.next(counts, section, index, direction, 6)
        return [to.section, to.index]
    }

    function test_left_and_right_go_round_the_section() {
        compare(go([8, 20], 0, 7, "right"), [0, 0])
        compare(go([8, 20], 0, 0, "left"), [0, 7])
        compare(go([8, 20], 1, 5, "right"), [1, 6], "on to the next row")
        compare(go([8, 20], 1, 19, "right"), [1, 0])
    }

    function test_up_and_down_keep_the_column() {
        compare(go([8, 20], 1, 3, "down"), [1, 9])
        compare(go([8, 20], 1, 9, "up"), [1, 3])
        compare(go([14, 20], 0, 2, "down"), [0, 8])
    }

    function test_down_from_the_last_row_goes_to_the_next_section() {
        compare(go([8, 20], 0, 3, "down"), [1, 3], "pinned row 1 (6, 7) is under 0..1 only")
        compare(go([8, 20], 0, 7, "down"), [1, 1])
        compare(go([8, 20], 1, 19, "down"), [0, 1], "all apps, round to the pinned ones")
        compare(go([8, 3], 0, 5, "down"), [1, 2], "a shorter row: its last tile")
    }

    function test_up_from_the_first_row_goes_to_the_previous_section() {
        compare(go([8, 20], 1, 4, "up"), [0, 7], "the pinned ones' last row has 6, 7: the last")
        compare(go([8, 20], 1, 1, "up"), [0, 7])
        compare(go([8, 20], 1, 0, "up"), [0, 6])
        compare(go([8, 18], 0, 2, "up"), [1, 14], "round to all apps' last row")
        compare(go([8, 20], 0, 2, "up"), [1, 19], "a shorter last row: its last tile")
    }

    function test_one_section_goes_round_itself() {
        compare(go([20], 0, 19, "down"), [0, 1])
        compare(go([20], 0, 1, "up"), [0, 19])
        compare(go([20], 0, 4, "up"), [0, 19])
        compare(go([18], 0, 4, "up"), [0, 16])
    }

    function test_empty_sections_are_skipped() {
        compare(go([0, 5], 1, 2, "down"), [1, 2])
        compare(go([8, 0], 0, 7, "down"), [0, 1])
    }
}
