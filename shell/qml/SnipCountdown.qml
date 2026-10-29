// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Snipping Tool's countdown (created by snip.cpp): a small number while
// the delay runs; it is gone before the photo is taken.
Window {
    property int seconds: 0
    visible: false
    width: 120
    height: 120
    color: "transparent"

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusOverlay
        color: Theme.dialog
        border.width: 1
        border.color: Theme.stroke
        Text {
            anchors.centerIn: parent
            text: seconds
            color: Theme.text
            font.pixelSize: 56
        }
    }
}
