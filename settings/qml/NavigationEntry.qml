// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A page in Settings' navigation list (Main.qml): its icon and title, the
// page in use with the accent's bar; a click opens it.
// Expressive: M3's navigation drawer item, at Vela's 36 rows (pointer
// density): the page in use a full pill of secondary container with its
// text in Medium, no bar; the others state layers of on surface.
Fill {
    id: entry
    property string title
    property string icon
    property bool selected: false
    signal activated()

    readonly property bool expressive: Theme.expressive
    readonly property real amount: entryMouse.pressed ? Theme.statePressed : entryMouse.containsMouse ? Theme.stateHover : 0
    height: 36
    radius: expressive ? height / 2 : Theme.radius
    color: expressive
        ? (selected ? Theme.mix(Theme.secondaryContainer, Theme.secondaryContainerText, amount)
                    : Qt.rgba(Theme.surfaceText.r, Theme.surfaceText.g, Theme.surfaceText.b, amount))
        : entryMouse.pressed ? Theme.subtlePressed : selected || entryMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest

    Rectangle {
        objectName: "selectionBar"
        visible: entry.selected && !entry.expressive
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: entryMouse.pressed ? 10 : 16
        radius: 1.5
        color: Theme.accentFill
    }
    Image {
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        width: 16
        height: 16
        source: "image://fileicon/" + encodeURIComponent(entry.icon)
        sourceSize: Qt.size(width, height)
    }
    Text {
        objectName: "title"
        x: 48
        anchors.verticalCenter: parent.verticalCenter
        text: entry.title
        color: entry.expressive && entry.selected ? Theme.secondaryContainerText : Theme.text
        font.pixelSize: Theme.fontBody
        font.weight: entry.expressive && entry.selected ? Theme.weightLabel : Font.Normal
    }
    MouseArea {
        id: entryMouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: entry.activated()
    }
}
