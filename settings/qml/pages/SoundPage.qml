// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// System > Sound: where sound goes and where it comes from, with the volume of
// each, like Windows 11.
Page {
    id: page
    Component.onCompleted: Audio.refresh()

    readonly property var defaultOutput: Audio.outputs.find(d => d.isDefault)
    readonly property var defaultInput: Audio.inputs.find(d => d.isDefault)

    component DeviceCard: Card {
        id: device
        required property var modelData
        required property string kind
        icon: kind === "output" ? (modelData.description.toLowerCase().indexOf("hdmi") >= 0 || modelData.description.toLowerCase().indexOf("displayport") >= 0 ? "video-display" : "audio-speakers") : "audio-input-microphone"
        title: modelData.description
        description: modelData.isDefault
            ? (modelData.defaultError
                ? qsTr("Couldn't verify the selected device. Controls are disabled.")
                : qsTr("Default device")) : ""
        clickable: !modelData.isDefault && !modelData.defaultPending
        onClicked: Audio.setDefault(kind, modelData.name)
        trailing: Rectangle {
            // The choice dot, like Windows RadioButtons.
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
    }

    component VolumeCard: Card {
        id: volumeCard
        required property var device
        required property string kind
        icon: device && device.muted ? "audio-volume-muted" : kind === "output" ? "audio-volume-high" : "audio-input-microphone"
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
                enabled: volumeCard.device && !volumeCard.device.mutePending
                    && !volumeCard.device.defaultPending && !volumeCard.device.defaultError
                onClicked: Audio.setMuted(volumeCard.kind, volumeCard.device.name, !volumeCard.device.muted)
            }
            Slider {
                anchors.verticalCenter: parent.verticalCenter
                width: 220
                enabled: volumeCard.device && !volumeCard.device.defaultPending && !volumeCard.device.defaultError
                to: 1
                stepSize: 0.02 // even numbers, like the volume keys and quick settings
                value: volumeCard.device ? Math.min(1, volumeCard.device.volume) : 0
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
        description: qsTr("pactl is needed (PipeWire or PulseAudio).")
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
    }

    CardGroup {
        title: qsTr("Advanced")
        LinkCard {
            visible: System.available("volume-mixer")
            icon: "view-media-equalizer"
            title: qsTr("Volume mixer")
            description: qsTr("Each app's volume, and the output it uses")
            onClicked: System.trigger("volume-mixer")
        }
    }
}
