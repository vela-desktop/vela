// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A taskbar button: the background fades on hover and a slight "bounce" when
// pressed. Under the icon, like on Windows 11, a gray dash if the app is open,
// longer and colored if it's the active one. With `draggable` it can be
// dragged sideways to move it (dropped says by how much).
Item {
    id: root

    property string tooltip: ""
    readonly property bool hovered: mouse.containsMouse
    property bool active: false
    property bool running: false
    property bool draggable: false
    readonly property bool dragging: mouse.dragging
    default property alias content: holder.data

    signal clicked()
    signal middleClicked()
    signal rightClicked(bool shift) // Shift+right click: the window menu
    signal dropped(real dx)
    signal wheeled(real delta) // angleDelta.y

    // While dragged it follows the mouse, above the other buttons.
    z: mouse.dragging ? 10 : 0
    transform: Translate { x: mouse.dragging ? mouse.dragX : 0 }

    width: 44
    height: 40

    Rectangle {
        anchors.fill: parent
        radius: Theme.radius
        color: mouse.pressed ? Theme.subtlePressed : Theme.subtleHover
        opacity: mouse.containsMouse || root.active ? 1 : 0

        Behavior on opacity {
            NumberAnimation { duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        }
    }

    Item {
        id: holder
        anchors.centerIn: parent
        width: 26
        height: 26
        scale: mouse.pressed ? 1 - 0.16 * Theme.motion : 1

        Behavior on scale {
            NumberAnimation { duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        }
    }

    // Indicator under the icon: app open / active (or Start menu open)
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 2
        height: 3
        radius: 1.5
        width: root.active ? 16 : root.running ? 6 : 0
        color: root.active ? Theme.accent : Theme.textSecondary
        opacity: root.active || root.running ? 1 : 0

        Behavior on width {
            enabled: Theme.motion > 0
            NumberAnimation { duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        }
        Behavior on color {
            ColorAnimation { duration: Theme.fast }
        }
        Behavior on opacity {
            NumberAnimation { duration: Theme.fast }
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        property real pressX: 0
        property real dragX: 0
        property bool dragging: false
        property bool dragged: false // releasing a drag isn't a click
        // In window coordinates: the button moves with the mouse, they don't.
        onPressed: mouse => {
            pressX = mapToItem(null, mouse.x, mouse.y).x
            dragX = 0
            dragged = false
        }
        onPositionChanged: mouse => {
            if (!pressed || !root.draggable || !(pressedButtons & Qt.LeftButton)) return
            dragX = mapToItem(null, mouse.x, mouse.y).x - pressX
            if (!dragging && Math.abs(dragX) > 8) dragging = true
        }
        onReleased: mouse => {
            if (dragging) {
                dragging = false
                dragged = true
                root.dropped(dragX)
            }
        }
        onWheel: wheel => root.wheeled(wheel.angleDelta.y)
        onClicked: mouse => {
            if (dragged) {
                dragged = false
                return
            }
            if (mouse.button === Qt.MiddleButton) {
                root.middleClicked()
            } else if (mouse.button === Qt.RightButton) {
                root.rightClicked(mouse.modifiers & Qt.ShiftModifier)
            } else {
                root.clicked()
            }
        }
    }
}
