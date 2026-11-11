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

    // Expressive (startmenu-variant=expressive, which StartMenu passes):
    // Vela's large corners (Theme.shapeLarge), M3's state layers of on
    // surface, the keyboard's tile in secondary container with its own state
    // layers (M3's selected list item; the shape is Vela's choice).
    property bool expressive: false
    readonly property real stateLayer: mouse.pressed ? Theme.statePressed : mouse.containsMouse ? Theme.stateHover : 0
    readonly property color layerColor: current ? Theme.secondaryContainerText : Theme.surfaceText

    Fill {
        objectName: "appTileFill"
        anchors.fill: parent
        anchors.margins: 3
        radius: root.expressive ? Theme.shapeLarge : Theme.radius
        color: root.expressive
            ? (root.current ? Theme.mix(Theme.secondaryContainer, Theme.secondaryContainerText, root.stateLayer)
                            : Qt.rgba(root.layerColor.r, root.layerColor.g, root.layerColor.b, root.stateLayer))
            : mouse.pressed ? Theme.subtlePressed : mouse.containsMouse || root.current ? Theme.subtleHover : Theme.subtleRest
        border.width: root.current && !root.expressive ? 1 : 0
        border.color: Theme.stroke
    }

    Column {
        objectName: "appTileContent"
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
            objectName: "appTileLabel"
            width: parent.width
            text: root.name
            color: root.expressive && root.current ? Theme.secondaryContainerText : Theme.text
            Behavior on color { enabled: root.expressive; ColorFade {} }
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
