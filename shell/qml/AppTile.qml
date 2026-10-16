// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A Start menu tile: icon and name.
Item {
    id: root

    required property int index
    required property string name
    required property string iconName
    required property string comment
    property bool current: false

    signal activated()
    signal contextRequested(real x, real y) // right button, in the tile's coordinates

    Fill {
        anchors.fill: parent
        anchors.margins: 3
        radius: Theme.radius
        color: mouse.pressed ? Theme.subtlePressed : mouse.containsMouse || root.current ? Theme.subtleHover : Theme.subtleRest
        border.width: root.current ? 1 : 0
        border.color: Theme.stroke
    }

    Column {
        anchors.centerIn: parent
        width: parent.width - 12
        spacing: 8
        scale: mouse.pressed ? 1 - 0.08 * Theme.motion : 1

        Behavior on scale { Eased {} }

        Icon {
            anchors.horizontalCenter: parent.horizontalCenter
            name: root.iconName
            size: 36
            smooth: true
            mipmap: true
        }

        Text {
            width: parent.width
            text: root.name
            color: Theme.text
            font.pixelSize: Theme.fontCaption
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: mouse => mouse.button === Qt.RightButton ? root.contextRequested(mouse.x, mouse.y) : root.activated()
    }
}
