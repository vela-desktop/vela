// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import Vela.Controls

// The veil over an output without the dialog: dark and blurred, it takes the
// pointer and keeps it.
Window {
    id: window
    color: "transparent"
    title: "Vela UAC veil"

    function updateBlur() {
        Effects.setBlur(window, [Qt.rect(0, 0, width, height)])
    }
    onWidthChanged: updateBlur()
    onHeightChanged: updateBlur()
    Component.onCompleted: updateBlur()

    Rectangle {
        anchors.fill: parent
        color: Theme.veil
        opacity: 0
        Component.onCompleted: opacity = Qt.binding(() => Request.closing ? 0 : 1)
        Behavior on opacity { Eased { duration: Request.closing ? Theme.normal : Theme.slow } }
    }
    // No clicking or scrolling underneath.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
    }
}
