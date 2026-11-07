// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

Rectangle {
    id: scene
    width: 320
    height: 112
    color: "black"

    property real pressScale: 1
    property real scrollOffset: 0
    property int pixelSize: 14
    property string fontFamily: Qt.application.font.family
    property color textColor: "white"

    Item {
        x: 24
        y: 32 - scene.scrollOffset
        scale: scene.pressScale
        transformOrigin: Item.TopLeft

        Text {
            objectName: "label"
            text: "Bitwarden — Dal tema"
            font.pixelSize: scene.pixelSize
            font.family: scene.fontFamily
            color: scene.textColor
            // Deliberately inherit the same process policy as real labels.
        }
    }
}
