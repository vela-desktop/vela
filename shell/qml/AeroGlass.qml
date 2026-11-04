// SPDX-FileCopyrightText: 2026 lowell
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Windows 7's glass over a panel (notifications-glass=1, Styles.notifications):
// a white line just inside the dark edge, a lit upper half and, if
// installed, the diagonal streaks of light (notifications-reflection). Put it
// as the last child of the panel's rectangle, which gives the radius.
Item {
    id: root
    property real radius: 0
    anchors.fill: parent
    visible: Styles.notifications.glass
    enabled: false // clicks go through

    Image { // the streaks, cut at the edge
        anchors { fill: parent; margins: 1 }
        visible: source != ""
        source: Styles.notifications.reflection
        fillMode: Image.PreserveAspectCrop
        opacity: Styles.notifications.reflectionOpacity
        asynchronous: true
    }
    Rectangle { // light on the upper half, fading out
        anchors { fill: parent; margins: 1 }
        radius: Math.max(0, root.radius - 1)
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#40FFFFFF" }
            GradientStop { position: 0.5; color: "#10FFFFFF" }
            GradientStop { position: 0.5; color: "#00FFFFFF" }
            GradientStop { position: 1.0; color: "#00FFFFFF" }
        }
    }
    Rectangle { // the white line inside the edge
        anchors { fill: parent; margins: 1 }
        radius: Math.max(0, root.radius - 1)
        color: "transparent"
        border.width: 1
        border.color: "#99FFFFFF"
    }
}
