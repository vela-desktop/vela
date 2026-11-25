// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../../qml"
import "../../../common/tests/colors.js" as Colors

// Files' navigation under the expressive controls (files-views-expressive):
// the navigation pane's place in use a full pill of secondary container
// with its label in on secondary container and Medium, no bar, the others
// state layers; the address bar and the search box M3's search bars, full
// pills without an outline, outlined in primary (2) while typing; the
// files' layer on surface container low with large top corners, no outline.
Item {
    id: root
    width: 900
    height: 600

    QtObject {
        id: tab
        property string location: "/home/ada/Documents"
        property bool isFolder: true
        property bool canGoBack: false
        property bool canGoForward: false
        property bool canGoUp: true
        property bool showHidden: false
        property string title: "Documents"
        function navigate(location) {}
        function openInNewTab(location) {}
        function back() {}
        function forward() {}
        function up() {}
        function refresh() {}
        function focusView() {}
        function search(text) {}
        function showError(text) {}
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

    TestCase {
        name: "FilesNavigationExpressive"
        when: windowShown

        function init() {
            if (!Theme.expressive)
                skip("with the expressive theme (the *-expressive ctest entry)")
            mouseMove(root, 890, 590)
            bar.editing = false
            root.forceActiveFocus()
        }

        function place(location) {
            return findChild(pane, "place:" + location)
        }

        function test_the_place_in_use_is_a_pill() {
            const docs = place("/home/ada/Documents")
            compare(docs.radius, docs.height / 2)
            tryVerify(() => Colors.same(docs.color, Theme.secondaryContainer), 5000)
            verify(!findChild(docs, "selectionBar").visible, "no bar")
            const label = findChild(docs, "label")
            verify(Colors.same(label.color, Theme.secondaryContainerText))
            compare(label.font.weight, Theme.weightLabel)
        }

        function test_the_others_are_state_layers() {
            const home = place("home:")
            compare(home.radius, home.height / 2)
            const layerOf = amount => Qt.rgba(Theme.surfaceText.r, Theme.surfaceText.g, Theme.surfaceText.b, amount)
            tryVerify(() => Colors.same(home.color, layerOf(0)), 5000)
            mouseMove(home, 60, 16)
            tryVerify(() => Colors.same(home.color, layerOf(Theme.stateHover)), 5000)
            verify(Colors.same(findChild(home, "label").color, Theme.text))
        }

        function test_search_bars() {
            for (const name of ["addressField", "searchBox"]) {
                const field = findChild(bar, name)
                compare(field.radius, field.height / 2, name)
                compare(field.border.width, 0, `${name}: no outline at rest`)
                tryVerify(() => Colors.same(field.color, Theme.control), 5000)
            }
            bar.focusAddress()
            const address = findChild(bar, "addressField")
            compare(address.border.width, 2)
            verify(Colors.same(address.border.color, Theme.primary))
            tryVerify(() => Colors.same(address.color, Theme.field), 5000)
            verify(Colors.same(Theme.field, Theme.surfaceContainerHighest))
            const search = findChild(bar, "searchBox")
            mouseClick(search, 20, 16)
            compare(search.border.width, 2)
            verify(Colors.same(search.border.color, Theme.primary))
            compare(address.border.width, 0, "the address no longer edited")
        }

        function test_the_files_layer() {
            verify(Colors.same(layer.color, Theme.surfaceContainerLow))
            compare(layer.topLeftRadius, Theme.shapeLarge)
            compare(layer.topRightRadius, Theme.shapeLarge)
            compare(layer.border.width, 0)
        }
    }
}
