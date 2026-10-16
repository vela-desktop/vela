// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Snipping Tool on one output (one per output, created by snip.cpp): the
// darkened photo of the output, and at the top the mode bar like Windows 11:
// Rectangle, Window, Full screen. Drag a rectangle, or click a window or the
// output; Esc cancels.
Window {
    id: root
    property string screenName
    visible: false
    color: "black"

    // The chosen rectangle (or the window under the mouse), in this output's
    // coordinates.
    property rect selection: Qt.rect(0, 0, 0, 0)
    property point origin
    property bool dragging: false
    readonly property bool hasSelection: selection.width > 0 && selection.height > 0

    // The topmost window under the point (this output's coordinates).
    function windowAt(x, y) {
        const gx = x + Screen.virtualX
        const gy = y + Screen.virtualY
        for (const w of Snip.windows) {
            if (gx >= w.x && gx < w.x + w.w && gy >= w.y && gy < w.y + w.h) {
                // Only the part on this output.
                const x0 = Math.max(w.x - Screen.virtualX, 0)
                const y0 = Math.max(w.y - Screen.virtualY, 0)
                const x1 = Math.min(w.x + w.w - Screen.virtualX, root.width)
                const y1 = Math.min(w.y + w.h - Screen.virtualY, root.height)
                return Qt.rect(x0, y0, x1 - x0, y1 - y0)
            }
        }
        return Qt.rect(0, 0, 0, 0)
    }
    function finish(r) {
        if (r.width >= 2 && r.height >= 2) {
            Snip.finish(root.screenName, r.x, r.y, r.width, r.height)
        }
    }

    Image {
        anchors.fill: parent
        source: root.screenName !== "" ? "image://snip/" + root.screenName : ""
        cache: false
        smooth: true
    }

    // The veil, except where the choice is being made.
    readonly property color veil: Qt.rgba(0, 0, 0, 0.45)
    Rectangle { color: root.veil; x: 0; y: 0; width: parent.width; height: root.hasSelection ? root.selection.y : parent.height }
    Rectangle {
        visible: root.hasSelection
        color: root.veil
        x: 0; y: root.selection.y + root.selection.height
        width: parent.width; height: parent.height - y
    }
    Rectangle {
        visible: root.hasSelection
        color: root.veil
        x: 0; y: root.selection.y; width: root.selection.x; height: root.selection.height
    }
    Rectangle {
        visible: root.hasSelection
        color: root.veil
        x: root.selection.x + root.selection.width; y: root.selection.y
        width: parent.width - x; height: root.selection.height
    }
    Rectangle {
        visible: root.hasSelection
        x: root.selection.x - 1
        y: root.selection.y - 1
        width: root.selection.width + 2
        height: root.selection.height + 2
        color: "transparent"
        border.width: 1
        border.color: "white"
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.CrossCursor
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: mouse => {
            if (mouse.button === Qt.RightButton) {
                Snip.cancel()
                return
            }
            if (Snip.mode === "rect") {
                root.origin = Qt.point(mouse.x, mouse.y)
                root.dragging = true
                root.selection = Qt.rect(mouse.x, mouse.y, 0, 0)
            }
        }
        onPositionChanged: mouse => {
            if (Snip.mode === "rect" && root.dragging) {
                root.selection = Qt.rect(Math.min(root.origin.x, mouse.x), Math.min(root.origin.y, mouse.y),
                    Math.abs(mouse.x - root.origin.x), Math.abs(mouse.y - root.origin.y))
            } else if (Snip.mode === "window") {
                root.selection = root.windowAt(mouse.x, mouse.y)
            } else if (Snip.mode === "screen") {
                root.selection = Qt.rect(0, 0, root.width, root.height)
            }
        }
        onExited: if (Snip.mode !== "rect") root.selection = Qt.rect(0, 0, 0, 0)
        onReleased: mouse => {
            if (mouse.button !== Qt.LeftButton) return
            if (Snip.mode === "rect") {
                root.dragging = false
                root.finish(root.selection)
            } else if (Snip.mode === "window") {
                root.finish(root.windowAt(mouse.x, mouse.y))
            } else {
                root.finish(Qt.rect(0, 0, root.width, root.height))
            }
        }
    }

    // The mode bar, at the top center.
    Rectangle {
        id: toolbar
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 24 }
        width: tools.width + 16
        height: 48
        radius: Theme.radiusOverlay
        color: Theme.dialog
        border.width: 1
        border.color: Theme.stroke

        Row {
            id: tools
            anchors.centerIn: parent
            spacing: 4
            Repeater {
                model: [
                    { mode: "rect", icon: "select-rectangular", tip: qsTr("Rectangle") },
                    { mode: "window", icon: "window", tip: qsTr("Window") },
                    { mode: "screen", icon: "view-fullscreen", tip: qsTr("Full screen") }
                ]
                delegate: Fill {
                    id: tool
                    required property var modelData
                    readonly property bool current: Snip.mode === modelData.mode
                    width: 40
                    height: 36
                    radius: Theme.radius
                    color: current || toolMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                    border.width: current ? 1 : 0
                    border.color: Theme.accent
                    Icon {
                        anchors.centerIn: parent
                        name: tool.modelData.icon
                        size: 20
                    }
                    MouseArea {
                        id: toolMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            Snip.mode = tool.modelData.mode
                            root.selection = Qt.rect(0, 0, 0, 0)
                        }
                    }
                }
            }
            Rectangle { width: 1; height: 24; anchors.verticalCenter: parent.verticalCenter; color: Theme.stroke }
            Fill {
                width: 40
                height: 36
                radius: Theme.radius
                color: closeMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                Text {
                    anchors.centerIn: parent
                    text: "✕"
                    color: Theme.text
                    font.pixelSize: 13
                }
                MouseArea {
                    id: closeMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Snip.cancel()
                }
            }
        }
    }

    Item {
        focus: true
        Keys.onEscapePressed: Snip.cancel()
        Keys.onPressed: event => {
            // Like Windows: Enter with a rectangle chosen confirms it.
            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && root.hasSelection) {
                root.finish(root.selection)
            }
        }
    }
}
