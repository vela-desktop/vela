// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Taskbar.Test

TestCase {
    name: "CalendarHover"
    when: windowShown
    ListModel { id: emptyHistory }
    Component { id: centerComponent; NotificationCenter {} }

    function test_adjacent_cells_and_weekday_header() {
        Notifications.history = emptyHistory
        const center = createTemporaryObject(centerComponent, null)
        verify(center)
        center.open()
        tryCompare(center, "visible", true)
        const grid = findChild(center.contentItem, "calendarDays")
        verify(grid)
        const cells = grid.children.filter(c => c.index !== undefined)
        compare(cells.length, 42)
        const first = cells[0]
        const second = cells[1]
        const firstFill = first.children[0]
        const secondFill = second.children[0]
        tryVerify(() => grid.width > 0)

        mouseMove(first, first.width / 2, first.height / 2)
        tryCompare(grid, "hoveredDay", first.index)
        tryVerify(() => firstFill.item && firstFill.item.color.a > 0)
        mouseMove(second, second.width / 2, second.height / 2)
        tryCompare(grid, "hoveredDay", second.index)
        if (Theme.fast > 0)
            verify(firstFill.item !== null, "the outgoing cell still has its own fade")
        tryVerify(() => secondFill.item && secondFill.item.color.a > 0)
        mouseMove(first, first.width / 2, first.height / 2)
        tryCompare(grid, "hoveredDay", first.index)
        mouseMove(grid, first.width / 2, 12)
        tryCompare(grid, "hoveredDay", -1)
        tryCompare(firstFill, "item", null)
        tryCompare(secondFill, "item", null)
        compare(cells.filter(c => c.children[0].item !== null).length, 1)
        const today = cells.find(c => c.today)
        verify(today && today.children[0].item !== null)
    }
}
