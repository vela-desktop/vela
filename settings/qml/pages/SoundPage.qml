// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import Vela.Audio
import Vela.Controls

// System > Sound, like Windows 11: where sound goes and where it comes
// from, the volume of each, the microphone's level, each device's
// properties and the volume mixer; then the system's sample rate and
// restarting audio (docs/audio.md §6.2, §8.1).
Page {
    id: page

    readonly property var defaultOutput: Audio.outputs.find(d => d.isDefault)
    readonly property var defaultInput: Audio.inputs.find(d => d.isDefault)
    // A device turned off has no outputs or inputs left: it is listed here
    // to turn it back on.
    readonly property var offDevices: Audio.devices.filter(d => d.off)

    // The input's level, only while the page is on screen: it opens the
    // microphone (docs/audio.md §7.2).
    LevelMeter {
        id: inputMeter
        audio: Audio
        kind: "input"
        name: Audio.defaultInput
        active: page.visible && root.visibility !== Window.Minimized && root.visibility !== Window.Hidden
    }

    component DeviceCard: Card {
        id: device
        required property var modelData
        required property string kind
        icon: modelData.icon
        title: modelData.description
        description: {
            if (modelData.isDefault && modelData.defaultError) {
                return qsTr("Couldn't verify the selected device. Controls are disabled.")
            }
            const parts = []
            if (modelData.isDefault) {
                parts.push(qsTr("Default device"))
            }
            if (modelData.battery >= 0) {
                parts.push(qsTr("Battery %1%").arg(modelData.battery))
            }
            return parts.join(" · ")
        }
        clickable: !modelData.isDefault && !modelData.defaultPending
        onClicked: Audio.setDefault(kind, modelData.name)
        trailing: Row {
            spacing: 4
            // The choice dot, like Windows RadioButtons.
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 20
                radius: 10
                color: device.modelData.isDefault ? Theme.accentFill : "transparent"
                border.width: device.modelData.isDefault ? 0 : 1
                border.color: Theme.controlStrokeStrong
                Rectangle {
                    visible: device.modelData.isDefault
                    anchors.centerIn: parent
                    width: 10
                    height: 10
                    radius: 5
                    color: Theme.accentText
                }
            }
            Item { width: 8; height: 1 }
            // The device's properties: the arrow of the cards that lead to a
            // page, as a button of its own.
            Rectangle {
                width: 32
                height: 32
                radius: Theme.radius
                color: propertiesMouse.pressed ? Theme.subtlePressed : propertiesMouse.containsMouse ? Theme.subtleHover : "transparent"
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Properties")
                Text {
                    anchors.centerIn: parent
                    text: "›"
                    color: Theme.textSecondary
                    font.pixelSize: 22
                }
                MouseArea {
                    id: propertiesMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.showSoundDevice(device.kind, device.modelData.name, device.modelData.device)
                }
            }
        }
    }

    component VolumeCard: Card {
        id: volumeCard
        required property var device
        required property string kind
        icon: device && device.muted ? (kind === "output" ? "audio-volume-muted" : "microphone-sensitivity-muted")
            : kind === "output" ? "audio-volume-high" : "audio-input-microphone"
        title: qsTr("Volume")
        description: device && device.defaultError
            ? qsTr("Couldn't verify the selected device. Controls are disabled.")
            : device && device.muteError
                ? qsTr("Couldn't change mute. Check the device state.")
                : ""
        trailing: Row {
            spacing: 8
            Button {
                subtle: true
                icon: volumeCard.device && volumeCard.device.muted ? (volumeCard.kind === "output" ? "audio-volume-muted" : "microphone-sensitivity-muted") : (volumeCard.kind === "output" ? "audio-volume-high" : "audio-input-microphone")
                Accessible.name: volumeCard.device && volumeCard.device.muted ? qsTr("Unmute") : qsTr("Mute")
                usable: volumeCard.device !== undefined && !volumeCard.device.mutePending
                    && !volumeCard.device.defaultPending && !volumeCard.device.defaultError
                onClicked: Audio.setMuted(volumeCard.kind, volumeCard.device.name, !volumeCard.device.muted)
            }
            Slider {
                anchors.verticalCenter: parent.verticalCenter
                width: 220
                enabled: volumeCard.device !== undefined && !volumeCard.device.defaultPending && !volumeCard.device.defaultError
                to: 1
                // Even numbers, like the volume keys and quick settings, or
                // the device's own positions when they are fewer.
                stepSize: volumeCard.device ? 1 / Math.min(50, volumeCard.device.intervals) : 0.02
                value: volumeCard.device ? volumeCard.device.volume : 0
                onMoved: value => Audio.setVolume(volumeCard.kind, volumeCard.device.name, value)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: 32
                text: volumeCard.device ? Math.round(volumeCard.device.volume * 100) : ""
                color: Theme.text
                font.pixelSize: Theme.fontBody
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    Card {
        visible: !Audio.available
        icon: "dialog-warning"
        title: qsTr("Sound isn't available")
        description: qsTr("PipeWire isn't running. Vela keeps trying to reach it.")
    }

    CardGroup {
        visible: Audio.available
        title: qsTr("Output")
        Text {
            text: qsTr("Choose where to play sound")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            bottomPadding: 4
        }
        Repeater {
            model: Audio.outputs
            delegate: DeviceCard { kind: "output" }
        }
        VolumeCard {
            visible: page.defaultOutput !== undefined
            device: page.defaultOutput
            kind: "output"
        }
    }

    CardGroup {
        visible: Audio.available && Audio.inputs.length > 0
        title: qsTr("Input")
        Text {
            text: qsTr("Choose a device for speaking or recording")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            bottomPadding: 4
        }
        Repeater {
            model: Audio.inputs
            delegate: DeviceCard { kind: "input" }
        }
        VolumeCard {
            visible: page.defaultInput !== undefined
            device: page.defaultInput
            kind: "input"
        }
        Card {
            visible: page.defaultInput !== undefined
            icon: "audio-input-microphone"
            title: qsTr("Input level")
            description: page.defaultInput && page.defaultInput.muted
                ? qsTr("The microphone is muted")
                : qsTr("Speak to test your microphone")
            trailing: LevelBar {
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                width: 220 + 8 + 32 + 8 + 32
                level: inputMeter.level
            }
        }
    }

    CardGroup {
        visible: Audio.available
        title: qsTr("Advanced")
        Repeater {
            model: page.offDevices
            delegate: LinkCard {
                required property var modelData
                icon: modelData.icon
                title: modelData.description
                description: qsTr("Turned off")
                onClicked: root.showSoundDevice("", "", modelData.name)
            }
        }
        LinkCard {
            icon: "view-media-equalizer"
            title: qsTr("Volume mixer")
            description: qsTr("Each app's volume, and the devices it uses")
            onClicked: root.navigate("volume-mixer")
        }
        Card {
            id: rateCard
            readonly property var choices: [{ text: qsTr("Automatic"), rate: 0 }]
                .concat(Audio.sampleRates.map(r => ({ text: qsTr("%1 Hz").arg(r), rate: r })))
                .concat(Audio.sampleRateMode === "custom" ? [{ text: qsTr("Custom"), rate: -1 }] : [])
            visible: Audio.sampleRateMode !== ""
            icon: "chronometer"
            title: qsTr("Sample rate")
            description: Audio.sampleRateMode === "automatic"
                ? qsTr("Sound plays at its own rate, without conversion")
                : Audio.sampleRateMode === "fixed"
                    ? qsTr("All sound is converted to %1 Hz").arg(Audio.sampleRate)
                    : qsTr("Set elsewhere: %1 Hz").arg(Audio.allowedRates.join(", "))
            trailing: Choice {
                model: rateCard.choices
                currentIndex: rateCard.choices.findIndex(c => c.rate === Audio.sampleRate)
                onChosen: index => {
                    if (rateCard.choices[index].rate >= 0) {
                        Audio.setSampleRate(rateCard.choices[index].rate)
                    }
                }
            }
        }
        Card {
            icon: "view-refresh"
            title: qsTr("Restart audio")
            description: qsTr("If sound is stuck. Sound stops for a few seconds, and apps reconnect.")
            trailing: Button {
                text: qsTr("Restart")
                usable: !Audio.restartPending
                onClicked: Audio.restartAudio()
            }
        }
    }
}
