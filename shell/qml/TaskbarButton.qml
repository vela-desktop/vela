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

    readonly property QtObject style: Styles.taskbar
    width: style.buttonWidth
    height: style.buttonHeight

    // A style's pictures (Windows 7's glass tiles, asset:): then the button
    // isn't drawn the Fluent way (no hover plate, no dash under the icon).
    readonly property bool pictured: style.tileImage !== ""
    BorderImage {
        anchors.fill: parent
        source: !root.running ? ""
            : root.active ? root.style.tileImageActive : root.hovered ? root.style.tileImageHover : root.style.tileImage
        visible: source != ""
        border { left: 3; right: 3; top: 3; bottom: 3 }
    }
    // An app that is not open, under the pointer: the picture's glow without
    // its two side lines (4 px cut off each side).
    Image {
        anchors.fill: parent
        visible: root.pictured && !root.running && root.hovered && source != ""
        source: root.style.tileImageIdleHover
        sourceClipRect: Qt.rect(4, 0, Math.max(1, sourceSize.width - 8), sourceSize.height)
        fillMode: Image.Stretch
    }
    // A style's tile under each open app, drawn: its color (the focused
    // app's), a glossy gradient up to a lighter top, a hairline.
    Rectangle {
        id: tile
        anchors.fill: parent
        visible: root.running && !root.pictured && (root.style.buttonFill.a > 0 || root.style.buttonStroke.a > 0)
        radius: root.style.buttonRadius
        readonly property color low: root.active ? root.style.buttonFillActive : root.style.buttonFill
        color: root.style.buttonGloss ? "transparent" : low
        gradient: root.style.buttonGloss ? gloss : null
        border.width: root.style.buttonStroke.a > 0 ? 1 : 0
        border.color: root.style.buttonStroke
        Gradient {
            id: gloss
            GradientStop { position: 0; color: Qt.lighter(tile.low, 1.6) }
            GradientStop { position: 0.5; color: Qt.lighter(tile.low, 1.25) }
            GradientStop { position: 0.51; color: tile.low }
            GradientStop { position: 1; color: Qt.darker(tile.low, 1.1) }
        }
    }

    Fill {
        anchors.fill: parent
        visible: !root.pictured
        radius: root.style.buttonRadius
        color: mouse.pressed ? Theme.subtlePressed : mouse.containsMouse || root.active ? Theme.subtleHover : Theme.subtleRest
    }

    Item {
        id: holder
        anchors.centerIn: parent
        width: root.style.iconSize
        height: root.style.iconSize
        scale: mouse.pressed && root.style.bounce ? 1 - 0.16 * Theme.motion : 1

        Behavior on scale { Eased {} }
    }

    // Indicator under the icon: app open / active (or Start menu open)
    Rectangle {
        visible: !root.pictured
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
            Eased {}
        }
        Behavior on color { ColorFade {} }
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
