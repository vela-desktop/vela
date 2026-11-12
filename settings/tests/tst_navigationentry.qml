// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"
import "../../common/tests/colors.js" as Colors

// Settings' navigation list (NavigationEntry.qml, Main.qml's delegate) in
// the default look: the page in use on the hover color with the accent's
// bar, the others subtle until hovered or pressed; a click asks for the page.
Item {
    id: root
    width: 320
    height: 200

    property string section: "system"

    Column {
        x: 8
        y: 8
        width: 280
        spacing: 2
        Repeater {
            model: ["home", "system", "network"]
            delegate: NavigationEntry {
                required property string modelData
                objectName: "navigation:" + modelData
                width: parent.width
                title: modelData
                icon: "preferences-system"
                selected: root.section === modelData
                onActivated: root.section = modelData
            }
        }
    }

    TestCase {
        name: "NavigationEntry"
        when: windowShown

        function entry(name) {
            return findChild(root, "navigation:" + name)
        }

        function init() {
            mouseMove(root, 315, 195)
            root.section = "system"
        }

        function test_the_page_in_use() {
            const system = entry("system")
            compare(system.height, 36)
            compare(system.radius, Theme.radius)
            tryVerify(() => Colors.same(system.color, Theme.subtleHover), 5000)
            const bar = findChild(system, "selectionBar")
            verify(bar.visible)
            compare(bar.width, 3)
            compare(bar.height, 16)
            verify(Colors.same(bar.color, Theme.accentFill))
            const title = findChild(system, "title")
            verify(Colors.same(title.color, Theme.text))
            compare(title.font.weight, Font.Normal)
            verify(!findChild(entry("home"), "selectionBar").visible)
        }

        function test_rest_hover_and_pressed() {
            const home = entry("home")
            tryVerify(() => Colors.same(home.color, Theme.subtleRest), 5000)
            mouseMove(home, 100, 18)
            tryVerify(() => Colors.same(home.color, Theme.subtleHover), 5000)
            mousePress(home, 100, 18)
            tryVerify(() => Colors.same(home.color, Theme.subtlePressed), 5000)
            mouseRelease(home, 100, 18)
            compare(root.section, "home")
            verify(home.selected)
            verify(findChild(home, "selectionBar").visible)
            verify(!findChild(entry("system"), "selectionBar").visible)
        }

        function test_the_bar_shrinks_while_pressed() {
            const system = entry("system")
            mousePress(system, 100, 18)
            compare(findChild(system, "selectionBar").height, 10)
            mouseRelease(system, 100, 18)
            compare(findChild(system, "selectionBar").height, 16)
        }
    }
}
