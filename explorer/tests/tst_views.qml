// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Files.Backend
import "../qml"
import "../../common/tests/colors.js" as Colors

// Every other file selected; a scroll by an odd number of rows hands each
// recycled delegate a file in the other state. Its color must be the new
// state's at once: a fade from the old one would be caught halfway
// (docs/visual-coherence.md §6.8).
Item {
    id: root
    width: 700
    height: 500

    FolderModel { id: folder; path: TestFolder }

    QtObject {
        id: tab
        property var model: folder
        property bool viewFocused: false
        property string renamingPath: ""
        property bool isTrash: false
        property bool writable: false
        property string location: TestFolder
        function focusView() {}
        function dragUrls(row) { return [] }
    }

    DetailsView { id: details; width: 600; height: 400; tab: tab; visible: false }
    IconView { id: icons; width: 300; height: 400; tab: tab; mode: "small"; visible: false } // one column

    TestCase {
        name: "FilesViews"
        when: windowShown

        function initTestCase() {
            tryCompare(folder, "count", 300, 5000)
            const even = []
            for (let i = 0; i < 300; i += 2) even.push(i)
            folder.selectRows(even, false)
            // Away from every row: no hover.
            mouseMove(root, 690, 490)
        }

        // The delegates on screen, with their Fill.
        function shown(view, flickable, name) {
            const out = []
            const top = flickable.contentY, bottom = top + flickable.height
            for (const item of flickable.contentItem.children) {
                if (item.selected === undefined || item.y + item.height <= top || item.y >= bottom) continue
                const fill = findChild(item, name)
                if (fill) out.push({ item: item, fill: fill })
            }
            return out
        }

        function expected(item) {
            return item.selected ? Theme.selection : Theme.subtleRest
        }

        function scrollAndCheck(view, flickable, name, step, perScreen) {
            view.visible = true
            const firstIndex = new Map() // delegate -> the file it was made for
            let reused = 0
            for (let pass = 0; pass < 6; ++pass) {
                tryVerify(() => shown(view, flickable, name).every(d => Colors.same(d.fill.color, expected(d.item))), 5000,
                          "settled before scrolling")
                for (const d of shown(view, flickable, name))
                    if (!firstIndex.has(d.item)) firstIndex.set(d.item, d.item.index)
                flickable.contentY += step
                flickable.forceLayout() // the new rows now, not at the next frame
                const now = shown(view, flickable, name)
                verify(now.length >= perScreen, "a full screen of delegates: " + now.length)
                for (const d of now) {
                    verify(Colors.same(d.fill.color, expected(d.item)),
                           "row " + d.item.index + " (" + (d.item.selected ? "selected" : "not selected") + ") is " + d.fill.color)
                    if (firstIndex.has(d.item) && firstIndex.get(d.item) !== d.item.index) ++reused
                }
            }
            verify(reused > 0, "the view recycled its delegates")
            view.visible = false
        }

        // The view's ListView or GridView.
        function flickable(view) {
            for (let i = 0; i < view.children.length; ++i)
                if (view.children[i].reuseItems !== undefined) return view.children[i]
            return null
        }

        function test_details_view() {
            scrollAndCheck(details, flickable(details), "rowFill", details.rowHeight * 25, 10)
        }

        function test_icon_view() {
            scrollAndCheck(icons, flickable(icons), "cellFill", icons.cellHeight * 25, 10)
        }
    }
}
