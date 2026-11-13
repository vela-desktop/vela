// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../../qml"
import "../../../common/tests/colors.js" as Colors

// Settings' navigation list under the expressive controls (the
// settings-controls-expressive test theme): M3's navigation drawer item, the
// page in use a full pill of secondary container with its title in on
// secondary container and Medium, no bar; the others state layers of on
// surface, the page in use's own over secondary container.
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
            model: ["home", "system"]
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
        name: "NavigationExpressive"
        when: windowShown

        function entry(name) {
            return findChild(root, "navigation:" + name)
        }
        function stateLayer(amount) {
            return Qt.rgba(Theme.surfaceText.r, Theme.surfaceText.g, Theme.surfaceText.b, amount)
        }

        function init() {
            if (!Theme.expressive)
                skip("with the expressive theme (the *-expressive ctest entries)")
            mouseMove(root, 315, 195)
            root.section = "system"
        }

        function test_the_page_in_use_is_a_secondary_container_pill() {
            const system = entry("system")
            compare(system.radius, system.height / 2)
            tryVerify(() => Colors.same(system.color, Theme.secondaryContainer), 5000)
            verify(!findChild(system, "selectionBar").visible, "no bar")
            const title = findChild(system, "title")
            verify(Colors.same(title.color, Theme.secondaryContainerText))
            compare(title.font.weight, Theme.weightLabel)
            mouseMove(system, 100, 18)
            tryVerify(() => Colors.same(system.color,
                Theme.mix(Theme.secondaryContainer, Theme.secondaryContainerText, Theme.stateHover)), 5000)
        }

        function test_the_others_are_state_layers() {
            const home = entry("home")
            tryVerify(() => Colors.same(home.color, stateLayer(0)), 5000)
            verify(Colors.same(findChild(home, "title").color, Theme.text))
            compare(findChild(home, "title").font.weight, Font.Normal)
            mouseMove(home, 100, 18)
            tryVerify(() => Colors.same(home.color, stateLayer(Theme.stateHover)), 5000)
            mousePress(home, 100, 18)
            tryVerify(() => Colors.same(home.color, stateLayer(Theme.statePressed)), 5000)
            mouseRelease(home, 100, 18)
            compare(root.section, "home")
            tryVerify(() => Colors.same(home.color,
                Theme.mix(Theme.secondaryContainer, Theme.secondaryContainerText, Theme.stateHover)), 5000)
        }
    }
}
