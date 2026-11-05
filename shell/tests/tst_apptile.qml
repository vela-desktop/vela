// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

Item {
    width: 320
    height: 240

    AppTile {
        id: tile
        x: 24
        y: 24
        width: 118
        height: 118
        index: 0
        name: "Bitwarden"
        iconName: "application-x-executable"
        comment: ""
    }
    SignalSpy { id: activated; target: tile; signalName: "activated" }
    SignalSpy { id: context; target: tile; signalName: "contextRequested" }

    TestCase {
        name: "AppTile"
        when: windowShown
        property int heldButton: Qt.NoButton
        readonly property var column: findChild(tile, "appTileContent")
        readonly property var label: findChild(tile, "appTileLabel")

        function initTestCase() {
            verify(column !== null)
            verify(label !== null)
        }

        function init() {
            activated.clear()
            context.clear()
            tryCompare(column, "scale", 1, 3000)
        }
        function cleanup() {
            if (heldButton !== Qt.NoButton) {
                mouseRelease(tile, -10, -10, heldButton)
                heldButton = Qt.NoButton
            }
        }

        function test_press_keeps_text_layout_data() {
            return [
                { tag: "left", button: Qt.LeftButton },
                { tag: "right", button: Qt.RightButton }
            ]
        }
        function test_press_keeps_text_layout(data) {
            const width = label.implicitWidth
            const height = label.implicitHeight
            const renderer = label.renderType
            compare(renderer, Text.QtRendering)
            heldButton = data.button
            mousePress(tile, 60, 60, data.button)
            tryCompare(column, "scale", 1 - 0.08 * Theme.motion, 3000)
            compare(activated.count, 0)
            compare(context.count, 0)
            compare(label.renderType, renderer)
            compare(label.implicitWidth, width)
            compare(label.implicitHeight, height)
            mouseRelease(tile, 60, 60, data.button)
            heldButton = Qt.NoButton
            compare(activated.count, data.button === Qt.LeftButton ? 1 : 0)
            compare(context.count, data.button === Qt.RightButton ? 1 : 0)
            if (data.button === Qt.RightButton) {
                compare(context.signalArguments[0][0], 60)
                compare(context.signalArguments[0][1], 60)
            }
            tryCompare(column, "scale", 1, 3000)
            compare(label.renderType, renderer)
            compare(label.implicitWidth, width)
            compare(label.implicitHeight, height)
        }
    }
}
