// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton

import QtQuick
import Vela.Shell.Test as ShellTest

QtObject {
    readonly property QtObject notifications: ShellTest.Styles.notifications
    property QtObject taskbar: QtObject {
        property int height: 48
        property string position: "bottom"
        readonly property bool vertical: position === "left" || position === "right"
        readonly property int thickness: vertical ? Math.max(height, 56) : height
        readonly property int edgeAnchor: position === "top" ? 1 : position === "left" ? 4 : position === "right" ? 8 : 2
        function towards(distance) {
            return position === "top" ? Qt.point(0, -distance) : position === "left" ? Qt.point(-distance, 0)
                : position === "right" ? Qt.point(distance, 0) : Qt.point(0, distance)
        }
        function origin(edges, width, height, screenWidth, screenHeight, left, top) {
            const area = usable(screenWidth, screenHeight)
            const x = (edges & 4) ? area.x + (left || 0) : (edges & 8) ? area.x + area.width - width
                : area.x + (area.width - width) / 2
            const y = (edges & 1) ? area.y + (top || 0) : (edges & 2) ? area.y + area.height - height
                : area.y + (area.height - height) / 2
            return Qt.point(x, y)
        }
        function usable(screenWidth, screenHeight) {
            return Qt.rect(position === "left" ? thickness : 0, position === "top" ? thickness : 0,
                screenWidth - (vertical ? thickness : 0), screenHeight - (vertical ? 0 : thickness))
        }
        // A panel's margin inside its window on `side` ("left", "top",
        // "right", "bottom"), anchored at `edges`: `gap` on the bar's side,
        // 12 at the screen's edges it is anchored to, `room` (its shadow's)
        // where it faces the screen.
        function pad(side, edges, gap, room) {
            const bit = side === "top" ? 1 : side === "bottom" ? 2 : side === "left" ? 4 : 8
            return side === position ? gap : (edges & bit) ? 12 : room
        }
        readonly property int trayCorner: position === "top" ? 1 | 8 : position === "left" ? 2 | 4 : 2 | 8
        property bool floating: false
        property bool fit: false
        property int edge: 0
        property int splitGap: 0
        property int splitRadius: 5
        property int margin: 0
        property int sideMargin: 0
        property int radius: 0
        property bool flush: false
        property color stroke: "transparent"
        property int buttonWidth: 44
        property int buttonHeight: 40
        property int ownButtonRadius: 4
        property int buttonRadius: 4
        property int iconSize: 26
        property int spacing: 4
        function corner(gap, own) { return own }
    }
}
