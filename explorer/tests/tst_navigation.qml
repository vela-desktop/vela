// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"
import "../../common/tests/colors.js" as Colors

// Files' navigation in the default look: the navigation pane (NavPane.qml)
// on the tests' places (FakePlaces.qml), the address bar (AddressBar.qml)
// on a fake tab, and the layer that holds them (FilesLayer.qml). Their
// colors against the live Theme, and what clicks and keys ask for.
Item {
    id: root
    width: 900
    height: 600

    // The tab the address bar drives: where it is and what it was asked.
    QtObject {
        id: tab
        property string location: "/home/ada/Documents/Notes"
        property bool isFolder: true
        property bool canGoBack: true
        property bool canGoForward: false
        property bool canGoUp: true
        property bool showHidden: false
        property string title: "Notes"
        property var asked: []
        property QtObject menus: QtObject {
            property var opened: []
            function open(entries, x, y) { opened = opened.concat([entries]) }
        }
        function ask(what) { asked = asked.concat([what]) }
        function navigate(location) { ask("navigate " + location) }
        function openInNewTab(location) { ask("tab " + location) }
        function back() { ask("back") }
        function forward() { ask("forward") }
        function up() { ask("up") }
        function refresh() { ask("refresh") }
        function focusView() { ask("focusView") }
        function search(text) { ask("search " + text) }
        function showError(text) { ask("error " + text) }
    }

    AddressBar {
        id: bar
        width: parent.width
        tab: tab
    }
    FilesLayer {
        id: layer
        y: 60
        width: parent.width
        height: parent.height - 60
        NavPane {
            id: pane
            width: 240
            height: parent.height
            current: "/home/ada/Documents"
        }
    }
    SignalSpy { id: navigated; target: pane; signalName: "navigate" }
    SignalSpy { id: newTab; target: pane; signalName: "openInNewTab" }
    SignalSpy { id: volume; target: pane; signalName: "openVolume" }
    SignalSpy { id: menu; target: pane; signalName: "contextMenu" }

    TestCase {
        name: "FilesNavigation"
        when: windowShown

        function init() {
            mouseMove(root, 890, 590)
            tab.location = "/home/ada/Documents/Notes"
            tab.isFolder = true
            tab.asked = []
            tab.menus.opened = []
            pane.current = "/home/ada/Documents"
            bar.editing = false
            for (const spy of [navigated, newTab, volume, menu])
                spy.clear()
        }

        function place(location) {
            return findChild(pane, "place:" + location)
        }

        // --- the navigation pane ---

        function test_places_in_order() {
            for (const location of ["home:", "/home/ada/Documents", "/home/ada/Pictures", "thispc:", "/", "/home/ada", "trash:"])
                verify(place(location), location)
            compare(findChild(place("/home/ada"), "label").text, "Ada")
            verify(place("/").y < place("/home/ada").y, "the drives, then the home folder, under This PC")
        }

        function test_the_place_in_use() {
            const docs = place("/home/ada/Documents")
            compare(docs.height, 32)
            compare(docs.radius, Theme.radius)
            tryVerify(() => Colors.same(docs.color, Theme.selection), 5000)
            verify(findChild(docs, "selectionBar").visible)
            verify(Colors.same(findChild(docs, "selectionBar").color, Theme.accentFill))
            compare(findChild(docs, "label").font.weight, Font.Normal)
            verify(Colors.same(findChild(docs, "label").color, Theme.text))
            verify(!findChild(place("home:"), "selectionBar").visible)
        }

        function test_hover_and_clicks() {
            const home = place("home:")
            tryVerify(() => Colors.same(home.color, Theme.restLayer), 5000)
            mouseMove(home, 60, 16)
            tryVerify(() => Colors.same(home.color, Theme.hoverLayer), 5000)
            mouseClick(home, 60, 16)
            compare(navigated.count, 1)
            compare(navigated.signalArguments[0][0], "home:")
            mouseClick(place("/home/ada/Pictures"), 60, 16, Qt.MiddleButton)
            compare(newTab.signalArguments[0][0], "/home/ada/Pictures")
        }

        function test_an_unmounted_drive_is_mounted() {
            let usb = null
            for (let i = 0; i < pane.contentItem.children[0].children.length; ++i) {
                const child = pane.contentItem.children[0].children[i]
                if (child.volume === "usb-1")
                    usb = child
            }
            verify(usb)
            compare(usb.opacity, 0.7)
            mouseClick(usb, 60, 16)
            compare(volume.count, 1)
            compare(volume.signalArguments[0][0], "usb-1")
            compare(navigated.count, 0)
        }

        function test_context_menu_pins() {
            mouseClick(place("/home/ada/Documents"), 60, 16, Qt.RightButton)
            compare(menu.count, 1)
            const texts = menu.signalArguments[0][0].map(e => e.text)
            verify(texts.indexOf("&Unpin from Quick access") >= 0, texts)
            mouseClick(place("/home/ada"), 60, 16, Qt.RightButton)
            verify(menu.signalArguments[1][0].map(e => e.text).indexOf("Pin to &Quick access") >= 0)
            mouseClick(place("trash:"), 60, 16, Qt.RightButton)
            verify(menu.signalArguments[2][0].map(e => e.text).indexOf("&Empty Recycle Bin") >= 0)
        }

        // --- the address bar ---

        function test_the_path_in_pieces() {
            compare(bar.crumbs.map(c => c.label), ["Ada", "Documents", "Notes"])
            compare(bar.crumbs.map(c => c.location), ["/home/ada", "/home/ada/Documents", "/home/ada/Documents/Notes"])
            tab.location = "/usr/share"
            compare(bar.crumbs.map(c => c.label), ["This PC", "Local Disk", "usr", "share"])
            compare(bar.crumbs[3].location, "/usr/share")
            tab.location = "trash:"
            compare(bar.crumbs.map(c => c.label), ["Recycle Bin"])
            tab.location = "home:"
            compare(bar.crumbs.map(c => c.label), ["Home"])
        }

        function test_a_piece_goes_there() {
            const docs = findChild(bar, "crumb1")
            tryVerify(() => Colors.same(docs.color, Theme.restLayer), 5000)
            mouseMove(docs, 10, 10)
            tryVerify(() => Colors.same(docs.color, Theme.hoverLayer), 5000)
            mouseClick(docs, 10, 10)
            compare(tab.asked, ["navigate /home/ada/Documents"])
            mouseClick(docs, 10, 10, Qt.MiddleButton)
            compare(tab.asked[1], "tab /home/ada/Documents")
        }

        function test_the_field_look_and_editing() {
            const field = findChild(bar, "addressField")
            compare(field.radius, Theme.radius)
            compare(field.border.width, 1)
            verify(Colors.same(field.border.color, Theme.controlStroke))
            tryVerify(() => Colors.same(field.color, Theme.control), 5000)
            bar.focusAddress()
            verify(bar.editing)
            const editor = findChild(bar, "addressEditor")
            compare(editor.text, "/home/ada/Documents/Notes")
            verify(Colors.same(field.border.color, Theme.accent))
            tryVerify(() => Colors.same(field.color, Theme.field), 5000)
            editor.text = "/srv"
            keyClick(Qt.Key_Return)
            compare(tab.asked, ["navigate /srv", "focusView"])
            verify(!bar.editing)
            bar.focusAddress()
            editor.text = "nowhere"
            keyClick(Qt.Key_Return)
            verify(tab.asked[2].startsWith("error Can't find \"nowhere\""))
            verify(bar.editing, "it stays to be corrected")
            keyClick(Qt.Key_Escape)
            verify(!bar.editing)
            compare(tab.asked[3], "focusView")
        }

        function test_the_search_box() {
            const box = findChild(bar, "searchBox")
            const input = findChild(bar, "searchInput")
            compare(box.radius, Theme.radius)
            compare(box.border.width, 1)
            mouseClick(box, 20, 16)
            verify(input.activeFocus)
            verify(Colors.same(box.border.color, Theme.accent))
            keyClick(Qt.Key_A)
            keyClick(Qt.Key_Return)
            compare(tab.asked, ["search a"])
            tab.isFolder = false
            compare(box.opacity, 0.5)
            verify(!input.enabled)
        }

        // --- the layer ---

        function test_the_files_layer() {
            verify(Colors.same(layer.color, Theme.layer))
            compare(layer.topLeftRadius, Theme.radiusOverlay)
            compare(layer.topRightRadius, Theme.radiusOverlay)
            compare(layer.bottomLeftRadius, 0)
            compare(layer.border.width, 1)
            verify(Colors.same(layer.border.color, Theme.layerStroke))
        }
    }
}
