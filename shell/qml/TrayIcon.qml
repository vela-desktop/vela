// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

Item {
    id: root
    required property var entry
    property int inset: 0
    property bool highlighted: false
    // Dragged 8 pixels or more, it follows the mouse; where it is let go
    // decides between the taskbar and the drawer (docs/taskbar.md §8.2).
    // The points are in its window's coordinates, also outside it.
    property bool draggable: false
    readonly property bool dragging: mouse.dragging
    signal invoked(int button)
    signal dragMoved(point position)
    signal dropped(point position)
    signal dragCanceled()

    z: mouse.dragging ? 10 : 0
    transform: Translate { x: mouse.dragging ? mouse.dragX : 0; y: mouse.dragging ? mouse.dragY : 0 }

    Accessible.role: Accessible.Button
    Accessible.name: entry.title
    Accessible.onPressAction: root.invoked(Qt.LeftButton)

    Fill {
        anchors { fill: parent; topMargin: root.inset; bottomMargin: root.inset }
        radius: Styles.taskbar.corner(8, Theme.radius)
        color: mouse.pressed ? Theme.subtlePressed
            : mouse.containsMouse || root.highlighted ? Theme.subtleHover : Theme.subtleRest
    }
    Image {
        anchors.centerIn: parent
        width: 18
        height: 18
        source: root.entry.icon
        sourceSize: Qt.size(width, height)
        smooth: true
        mipmap: true
    }
    Rectangle {
        visible: root.entry.attention
        width: 6
        height: 6
        radius: 3
        color: Theme.accent
        anchors { right: parent.right; top: parent.top; rightMargin: 6; topMargin: root.inset + 4 }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        property point pressPoint
        property real dragX: 0
        property real dragY: 0
        property bool dragging: false
        property bool dragged: false // releasing a drag isn't a click
        onPressed: event => {
            pressPoint = mapToItem(null, event.x, event.y)
            dragX = 0
            dragY = 0
            dragged = false
        }
        onPositionChanged: event => {
            if (!pressed || !root.draggable || !(pressedButtons & Qt.LeftButton)) return
            const p = mapToItem(null, event.x, event.y)
            dragX = p.x - pressPoint.x
            dragY = p.y - pressPoint.y
            if (!dragging && Math.hypot(dragX, dragY) > 8) {
                dragging = true
                root.clearTooltip()
            }
            if (dragging) root.dragMoved(p)
        }
        onReleased: event => {
            if (!dragging) return
            // While it still follows the mouse: the point is where it is.
            const p = mapToItem(null, event.x, event.y)
            dragging = false
            dragged = true
            root.dropped(p)
        }
        onCanceled: {
            if (dragging) root.dragCanceled()
            dragging = false
        }
        onClicked: event => {
            if (dragged) {
                dragged = false
                return
            }
            root.invoked(event.button)
        }
        onWheel: event => Tray.scroll(root.entry.row, event.angleDelta.y)
        onContainsMouseChanged: {
            if (containsMouse && root.entry.title !== "") delay.restart()
            else root.clearTooltip()
        }
    }
    Timer {
        id: delay
        interval: 500
        onTriggered: {
            const window = root.Window.window
            const center = window.x - window.screen.virtualX + root.mapToItem(null, root.width / 2, 0).x
            Menus.tooltip = { owner: root, text: root.entry.title, center: center, screen: window.screen.name }
        }
    }
    function clearTooltip() {
        delay.stop()
        if (Menus.tooltip && Menus.tooltip.owner === root) Menus.tooltip = null
    }
    Component.onDestruction: clearTooltip()
}
