// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The volume indicator, like Windows 11's: at the bottom in the middle, above
// the taskbar, for a moment after a volume key, the knob or the wheel on the
// taskbar's volume icon. The wheel on an app's taskbar button shows that app's
// volume instead, with its icon. It can be used too: dragged, the wheel, a
// click on the icon mutes. It stays while the mouse is on it. The microphone
// mute key shows whether the microphone is on, once the system confirms it.
Window {
    id: root
    visible: false
    width: 232
    height: 64 // the pill and the room it rises from
    color: "transparent"

    // What it shows: "" the system volume, "microphone", or an app's key
    // (Audio.apps).
    property string target: ""
    readonly property bool microphone: target === "microphone"
    readonly property var app: target !== "" && !microphone ? Audio.apps.find(a => a.key === target) || null : null
    readonly property real volume: app ? app.volume : Audio.volume
    readonly property bool muted: microphone ? Audio.inputMuted : app ? app.muted : Audio.muted

    Connections {
        target: Sound
        function onOsdRequested(what) {
            // Quick settings already shows the system volume.
            if (what === "" && Menus.quickSettingsOpen) {
                return
            }
            root.target = what
            root.show()
        }
    }
    function setVolume(value) {
        if (app) {
            if (app.muted) Audio.setAppMuted(app.key, false)
            Audio.setAppVolume(app.key, value)
        } else if (!microphone) {
            if (Audio.muted) Audio.setMuted("output", Audio.defaultOutput, false)
            Audio.setVolume("output", Audio.defaultOutput, value)
        }
    }
    function step(direction) {
        if (app) {
            Audio.stepAppVolume(app.key, direction)
        } else if (!microphone) {
            Audio.stepVolume("output", Audio.defaultOutput, direction)
        }
    }
    function toggleMute() {
        if (app) {
            Audio.setAppMuted(app.key, !app.muted)
        } else if (microphone) {
            Sound.press("mic-mute")
        } else {
            Audio.setMuted("output", Audio.defaultOutput, !Audio.muted)
        }
    }

    function show() {
        if (!visible) {
            Menus.placeOnTargetScreen(root)
            visible = true
            Effects.setBlur(root, [Qt.rect(pill.x, 12, pill.width, pill.height)])
            pill.opacity = 0
            slide.y = 12 * Theme.motion
            fade.stop()
            appear.restart()
        } else if (fade.running) {
            fade.stop()
            appear.restart()
        }
        hideTimer.restart()
    }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: pill; property: "opacity"; to: 1; duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        NumberAnimation { target: slide; property: "y"; to: 0; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }
    SequentialAnimation {
        id: fade
        NumberAnimation { target: pill; property: "opacity"; to: 0; duration: Theme.normal }
        ScriptAction { script: root.visible = false }
    }
    Timer {
        id: hideTimer
        interval: 2000
        onTriggered: {
            if (hover.hovered || barMouse.pressed) {
                restart()
            } else {
                fade.start()
            }
        }
    }

    // The wheel: a step every notch (120), touchpads add up their small ones.
    property real wheelRest: 0
    function wheel(delta) {
        wheelRest += delta
        while (Math.abs(wheelRest) >= 120) {
            step(wheelRest > 0 ? 1 : -1)
            wheelRest -= wheelRest > 0 ? 120 : -120
        }
        hideTimer.restart()
    }

    Rectangle {
        id: pill
        x: 0
        y: 12
        width: parent.width
        height: 52
        radius: Theme.radiusOverlay
        color: Theme.surface
        border.width: 1
        border.color: Theme.stroke
        transform: Translate { id: slide }

        HoverHandler { id: hover }
        MouseArea {
            anchors.fill: parent
            onWheel: wheel => root.wheel(wheel.angleDelta.y)
        }

        // The speaker (or the app's icon): a click mutes, like the one in
        // quick settings.
        Item {
            id: speaker
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 36
            height: 36
            Rectangle {
                anchors.fill: parent
                radius: Theme.radius
                color: Theme.subtleHover
                opacity: speakerMouse.containsMouse ? 1 : 0
            }
            Image {
                anchors.centerIn: parent
                width: 18
                height: 18
                opacity: root.app && root.app.muted ? 0.45 : 1
                source: Theme.icons + encodeURIComponent(root.app ? root.app.icon
                    : root.microphone ? (Audio.inputMuted ? "microphone-sensitivity-muted" : "audio-input-microphone")
                    : Audio.iconName)
                sourceSize: Qt.size(width, height)
            }
            Image {
                visible: !!root.app && root.app.muted
                anchors { right: parent.right; bottom: parent.bottom; margins: 5 }
                width: 12
                height: 12
                source: Theme.icons + "audio-volume-muted"
                sourceSize: Qt.size(width, height)
            }
            MouseArea {
                id: speakerMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    root.toggleMute()
                    hideTimer.restart()
                }
            }
        }

        // The microphone: on or off, no slider.
        Text {
            visible: root.microphone
            anchors { left: speaker.right; leftMargin: 10; right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
            text: Audio.inputMuted ? qsTr("Microphone off") : qsTr("Microphone on")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }

        Item {
            id: bar
            visible: !root.microphone
            readonly property real shown: Math.max(0, Math.min(1, root.volume))
            anchors { left: speaker.right; leftMargin: 10; right: number.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
            height: 20
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: 2
                color: Theme.stroke
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width * bar.shown
                height: 4
                radius: 2
                color: root.muted ? Theme.textSecondary : Theme.accent
            }
            Rectangle {
                x: parent.width * bar.shown - width / 2
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                radius: 9
                color: Theme.popup
                border.width: 1
                border.color: Theme.stroke
                Rectangle {
                    anchors.centerIn: parent
                    width: barMouse.pressed ? 8 : 10
                    height: width
                    radius: width / 2
                    color: root.muted ? Theme.textSecondary : Theme.accent
                }
            }
            MouseArea {
                id: barMouse
                anchors { fill: parent; margins: -8 }
                function update(x) {
                    root.setVolume(Math.max(0, Math.min(1, (x - 8) / bar.width)))
                    hideTimer.restart()
                }
                onPressed: mouse => update(mouse.x)
                onPositionChanged: mouse => { if (pressed) update(mouse.x) }
                onWheel: wheel => root.wheel(wheel.angleDelta.y)
            }
        }

        Text {
            id: number
            visible: !root.microphone
            anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
            width: 28 // "100" without the bar moving
            horizontalAlignment: Text.AlignRight
            text: Math.round(root.volume * 100)
            color: root.muted ? Theme.textSecondary : Theme.text
            font.pixelSize: Theme.fontBody
            font.features: { "tnum": 1 }
        }
    }
}
