// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

Item {
    id: root
    required property var entry
    property int inset: 0
    property bool highlighted: false
    signal invoked(int button)

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
        onClicked: event => root.invoked(event.button)
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
