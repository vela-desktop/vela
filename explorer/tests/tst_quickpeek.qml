// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Files.Backend
import "../qml"

// The quick preview's zoom: the wheel and a pinch zoom around the pointer
// between 1x and 8x, a drag pans without leaving the picture, a double click
// fits it again, and another item starts fitted.
Item {
    id: root
    width: 700
    height: 500

    // A folder view behind, like in the window: the wheel must not reach it.
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
        function zoom(step) {}
        function pressItem(row, button, modifiers) {}
        function releaseItem(row, button, modifiers) {}
    }
    DetailsView { id: details; anchors.fill: parent; tab: tab }

    QuickPeek { id: peek; tab: null }

    TestCase {
        name: "QuickPeekZoom"
        when: windowShown

        property var box
        property var content

        function init() {
            tryCompare(folder, "count", 300, 5000)
            mouseClick(details, 200, 60) // a file chosen with the mouse, as before Space
            peek.show(TestImage)
            tryVerify(() => peek.kind === "image")
            box = findChild(peek, "zoomBox")
            content = findChild(peek, "zoomContent")
            verify(box && box.visible)
        }
        function cleanup() { peek.path = ""; peek.hide() }

        // The point of the picture under (x, y) of the box.
        function under(x, y) {
            return Qt.point((x - content.x) / box.zoom, (y - content.y) / box.zoom)
        }

        function test_wheel_zooms_around_the_pointer() {
            const x = box.width * 0.7, y = box.height * 0.3
            const before = under(x, y)
            mouseWheel(box, x, y, 0, 120)
            verify(box.zoom > 1.1)
            const after = under(x, y)
            fuzzyCompare(after.x, before.x, 0.5)
            fuzzyCompare(after.y, before.y, 0.5)
            mouseWheel(box, x, y, 0, -120)
            fuzzyCompare(box.zoom, 1, 0.001)
        }

        // The folder view's Flickable, behind the preview.
        function listOf(item) {
            if (item.contentY !== undefined && item.contentHeight !== undefined) return item
            for (const child of item.children) {
                const found = listOf(child)
                if (found) return found
            }
            return null
        }

        function test_wheel_and_clicks_stay_in_the_preview() {
            const list = listOf(details)
            verify(list && list.contentHeight > list.height)
            for (const p of [[box.width / 2, box.height / 2], [5, 5]]) { // the picture, the backdrop
                const at = box.mapToItem(root, p[0], p[1])
                mouseWheel(root, at.x, at.y, 0, -360)
                compare(list.contentY, 0)
            }
            mouseWheel(root, 5, 5, 0, -360) // the backdrop, outside the card
            compare(list.contentY, 0)
            mouseClick(root, 5, root.height - 5)
            verify(!peek.visible) // a click outside closes it, and goes no further
        }

        function test_zoom_stays_between_1x_and_8x() {
            for (let i = 0; i < 30; ++i) mouseWheel(box, 10, 10, 0, 240)
            compare(box.zoom, 8)
            for (let i = 0; i < 30; ++i) mouseWheel(box, 10, 10, 0, -240)
            compare(box.zoom, 1)
            compare(content.x, 0)
            compare(content.y, 0)
        }

        function test_drag_pans_inside_the_picture() {
            const w = box.width, h = box.height
            mouseWheel(box, w / 2, h / 2, 0, 480)
            const x = content.x, y = content.y
            mouseDrag(box, w / 2, h / 2, 40, 30)
            verify(content.x > x && content.y > y)
            mouseDrag(box, w / 2, h / 2, w * 4, h * 4) // far past the edge
            compare(content.x, 0)
            compare(content.y, 0)
            mouseDrag(box, w / 2, h / 2, -w * 4, -h * 4)
            fuzzyCompare(content.x, w - w * box.zoom, 0.5)
            fuzzyCompare(content.y, h - h * box.zoom, 0.5)
        }

        function test_double_click_fits_again() {
            mouseWheel(box, 50, 50, 0, 480)
            verify(box.zoom > 1)
            mouseDoubleClickSequence(box, 50, 50)
            compare(box.zoom, 1)
            compare(content.x, 0)
        }

        function test_space_again_and_again_shows_the_item() {
            for (let i = 0; i < 6; ++i) {
                peek.hide()
                peek.show(TestImage)
                verify(peek.result !== null && peek.kind === "image", "after " + (i + 1) + " reopenings")
            }
        }

        function test_another_item_starts_fitted() {
            mouseWheel(box, 50, 50, 0, 480)
            peek.path = TestImage + ".other"
            compare(box.zoom, 1)
            compare(content.x, 0)
        }

        function test_pinch_zooms() {
            const y = box.height / 2, cx = box.width / 2
            const touch = touchEvent(box)
            touch.press(0, box, cx - 40, y).press(1, box, cx + 40, y).commit()
            for (let d = 40; d <= 160; d += 20) {
                touch.move(0, box, cx - d, y).move(1, box, cx + d, y).commit()
                wait(10)
            }
            touch.release(0, box, cx - 160, y).release(1, box, cx + 160, y).commit()
            verify(box.zoom > 1.5, "zoom after spreading two fingers: " + box.zoom)
        }

        function test_pdf_goes_page_by_page() {
            if (TestPdf === "") skip("built without QtPdf")
            peek.show(TestPdf)
            tryCompare(peek, "pages", 3, 5000)
            const pager = findChild(peek, "pager")
            verify(pager.visible)
            compare(peek.page, 0)
            mouseWheel(box, 50, 50, 0, 480)
            mouseClick(pager.children[2])
            compare(peek.page, 1)
            compare(box.zoom, 1, "a new page starts fitted")
            peek.pageBy(5)
            compare(peek.page, 2)
            mouseClick(pager.children[0])
            compare(peek.page, 1)
            peek.path = TestImage
            compare(peek.page, 0)
            tryVerify(() => !pager.visible)
        }
    }
}
