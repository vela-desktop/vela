// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Caps Lock on or off, for a moment, each time a key changes it
// (docs/keyboard.md §8.3): a small pill where the volume indicator appears,
// whose place it takes, as KDE and macOS show it. The compositor says when
// ("caps-lock on|off"), only for a real change of the Lock state: with Caps
// Lock mapped to another key nothing comes, and caps-lock-osd=no turns it
// off there. The mouse goes through it; a screen reader hears it as an
// alert.
//
// Expressive: a full pill on surface container without outline, like the
// volume indicator's.
Window {
    id: root
    readonly property bool expressive: Theme.expressive
    visible: false
    // The pill with room for its shadow around it, cut where the window
    // meets the taskbar: the pill rests 12 units from it (VolumeOsd.qml).
    readonly property real room: shadow.reach
    readonly property bool fromTop: Styles.taskbar.position === "top"
    width: room + motion.width + room
    height: room + motion.height + 12
    color: "transparent"

    property bool capsOn: false
    // How long it stays, before it fades.
    readonly property alias hold: hideTimer.interval

    Connections {
        target: Shell
        function onCapsLockChanged(on) {
            root.capsOn = on
            root.show()
        }
    }
    // The volume indicator takes the place back.
    Connections {
        target: Sound
        function onOsdRequested() {
            if (motion.opened) {
                motion.close()
            }
        }
    }

    // Shown again while leaving, it turns back from where it is.
    function show() {
        if (!motion.opened) {
            if (!motion.closing) {
                Shell.placeOnScreen(root, Shell.primaryScreen)
            }
            visible = true
            motion.open()
            Effects.setBlur(root, [motion.visibleArea])
        }
        hideTimer.restart()
    }

    // A second and a half, as long as Vela's durations are.
    Timer {
        id: hideTimer
        interval: Math.round(1500 * Theme.duration)
        onTriggered: motion.close()
    }

    // It rises 12 units, and leaves the same way.
    PanelMotion {
        id: motion
        x: root.room
        y: root.fromTop ? 12 : root.room
        width: Math.max(180, row.implicitWidth + 40)
        height: 52
        offset: Qt.point(0, root.fromTop ? -12 : 12)
        onClosed: root.visible = false
        onVisibleAreaChanged: Effects.setBlur(root, [visibleArea])

        Shadow {
            id: shadow
            target: pill
            radius: pill.radius
        }

        Rectangle {
            id: pill
            objectName: "capsLockPill"
            anchors.fill: parent
            radius: root.expressive ? height / 2 : Theme.radiusOverlay
            color: Theme.surface
            border.width: root.expressive ? 0 : 1
            border.color: Theme.stroke
            Accessible.role: Accessible.AlertMessage
            Accessible.name: label.text

            Row {
                id: row
                anchors.centerIn: parent
                spacing: 10
                Icon {
                    objectName: "capsLockIcon"
                    anchors.verticalCenter: parent.verticalCenter
                    size: 18
                    name: root.capsOn ? "keyboard-caps-locked" : "keyboard-caps-disabled"
                }
                Text {
                    id: label
                    objectName: "capsLockLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.capsOn ? qsTr("Caps Lock on") : qsTr("Caps Lock off")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
            }
        }
    }
}
