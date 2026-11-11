// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The desktop name, in the center of the output for a moment, when switching
// desktops (Win+Ctrl+arrows), like Windows 11. Expressive: a full pill on
// surface container without outline, the name in the emphasis weight.
Window {
    id: root
    readonly property bool expressive: Theme.expressive
    visible: false
    // The pill with room for its shadow around it (docs/visual-coherence.md
    // §6.9).
    readonly property real room: shadow.reach
    width: room + motion.width + room
    height: room + motion.height + room
    color: "transparent"

    property int last: -1 // not a binding: it's compared with the new one
    Component.onCompleted: last = Desktops.current

    Connections {
        target: Desktops
        function onChanged() {
            if (Desktops.current !== root.last) {
                root.last = Desktops.current
                // Task View already shows the desktops.
                if (!Menus.isOpen && !Menus.taskViewOpen) {
                    root.show()
                }
            }
        }
    }

    // Shown again while leaving, it turns back from where it is.
    function show() {
        Shell.placeOnScreen(root, Shell.primaryScreen)
        visible = true
        motion.open()
        Effects.setBlur(root, [motion.visibleArea])
        hideTimer.restart()
    }
    Timer { id: hideTimer; interval: 900; onTriggered: motion.close() }

    // It only fades.
    PanelMotion {
        id: motion
        x: root.room
        y: root.room
        width: label.implicitWidth + 64
        height: 64
        onClosed: root.visible = false
        onVisibleAreaChanged: Effects.setBlur(root, [visibleArea])

        Shadow {
            id: shadow
            target: pill
            radius: pill.radius
        }

        Rectangle {
            id: pill
            objectName: "desktopPill"
            anchors.fill: parent
            radius: root.expressive ? height / 2 : Theme.radiusOverlay
            color: Theme.surface
            border.width: root.expressive ? 0 : 1
            border.color: Theme.stroke
            Text {
                id: label
                anchors.centerIn: parent
                text: Desktops.names[Desktops.current] || ""
                color: Theme.text
                font.pixelSize: 20
                font.weight: Theme.weightEmphasis
            }
        }
    }
}
