// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import Vela.Audio
import Vela.Controls

// System > Sound > Volume mixer, like Windows 11: the system's output and
// input, then every app with its volume, mute, level, and the output and
// input it uses; "Reset" sends every app back to the defaults
// (docs/audio.md §6.2).
Page {
    id: page

    readonly property bool shown: page.visible && root.visibility !== Window.Minimized && root.visibility !== Window.Hidden
    readonly property var defaultOutput: Audio.outputs.find(d => d.isDefault)
    readonly property var defaultInput: Audio.inputs.find(d => d.isDefault)

    // Endpoints as choices; with `follow`, "Default" first (an app that
    // follows the system).
    function choices(list, follow) {
        const out = follow ? [{ text: qsTr("Default"), name: "" }] : []
        for (const e of list) {
            out.push({ text: e.description, name: e.name })
        }
        return out
    }

    component SystemCard: Card {
        id: system
        required property string kind
        readonly property var device: kind === "output" ? page.defaultOutput : page.defaultInput
        readonly property var options: page.choices(kind === "output" ? Audio.outputs : Audio.inputs, false)
        icon: device && device.muted ? (kind === "output" ? "audio-volume-muted" : "microphone-sensitivity-muted")
            : kind === "output" ? "audio-volume-high" : "audio-input-microphone"
        title: kind === "output" ? qsTr("Output device") : qsTr("Input device")
        trailing: Choice {
            model: system.options
            usable: system.device === undefined || !system.device.defaultPending
            currentIndex: system.device ? system.options.findIndex(o => o.name === system.device.name) : -1
            onChosen: index => Audio.setDefault(system.kind, system.options[index].name)
        }
        contentItem: Row {
            visible: system.device !== undefined
            spacing: 8
            Button {
                subtle: true
                icon: system.icon
                Accessible.name: system.device && system.device.muted ? qsTr("Unmute") : qsTr("Mute")
                usable: system.device !== undefined && !system.device.mutePending && !system.device.defaultError
                onClicked: Audio.setMuted(system.kind, system.device.name, !system.device.muted)
            }
            Slider {
                anchors.verticalCenter: parent.verticalCenter
                width: system.width - 32 - 32 - 8 - 40 - 16
                to: 1
                stepSize: system.device ? 1 / Math.min(50, system.device.intervals) : 0.02
                value: system.device ? system.device.volume : 0
                onMoved: value => Audio.setVolume(system.kind, system.device.name, value)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: 32
                text: system.device ? Math.round(system.device.volume * 100) : ""
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
        title: qsTr("System")
        SystemCard { kind: "output" }
        SystemCard {
            visible: Audio.inputs.length > 0
            kind: "input"
        }
    }

    CardGroup {
        visible: Audio.available
        title: qsTr("Apps")
        Card {
            visible: Audio.apps.length === 0
            icon: "applications-multimedia"
            title: qsTr("No apps are playing or recording sound")
            description: qsTr("Apps appear here while they use sound.")
        }
        Repeater {
            model: Audio.apps
            delegate: Card {
                id: app
                required property var modelData
                readonly property var outputs: page.choices(Audio.outputs, true)
                readonly property var inputs: page.choices(Audio.inputs, true)
                icon: "image://fileicon/" + encodeURIComponent(modelData.icon)
                title: modelData.name
                description: modelData.playing && modelData.recording ? qsTr("Playing and recording")
                    : modelData.recording ? qsTr("Recording") : ""

                LevelMeter {
                    id: appLevel
                    audio: Audio
                    kind: "app"
                    name: app.modelData.key
                    active: page.shown && app.modelData.playing
                }

                trailing: Row {
                    visible: app.modelData.playing
                    spacing: 8
                    Button {
                        subtle: true
                        icon: app.modelData.muted ? "audio-volume-muted" : "audio-volume-high"
                        Accessible.name: app.modelData.muted ? qsTr("Unmute") : qsTr("Mute")
                        onClicked: Audio.setAppMuted(app.modelData.key, !app.modelData.muted)
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 220
                        Slider {
                            width: parent.width
                            to: 1
                            stepSize: 0.02
                            value: app.modelData.volume
                            onMoved: value => Audio.setAppVolume(app.modelData.key, value)
                        }
                        LevelBar {
                            x: 10
                            width: parent.width - 20
                            height: 2
                            level: appLevel.level
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 32
                        text: Math.round(app.modelData.volume * 100)
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        horizontalAlignment: Text.AlignRight
                    }
                }
                // Under the name, like the text of the card.
                contentItem: Flow {
                    x: 36
                    width: app.width - 32 - 36
                    spacing: 16
                    Row {
                        visible: app.modelData.playing
                        spacing: 8
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Output device")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                        }
                        Choice {
                            model: app.outputs
                            currentIndex: app.outputs.findIndex(o => o.name === app.modelData.output)
                            onChosen: index => Audio.setAppOutput(app.modelData.key, app.outputs[index].name)
                        }
                    }
                    Row {
                        visible: app.modelData.recording
                        spacing: 8
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Input device")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                        }
                        Choice {
                            model: app.inputs
                            currentIndex: app.inputs.findIndex(o => o.name === app.modelData.input)
                            onChosen: index => Audio.setAppInput(app.modelData.key, app.inputs[index].name)
                        }
                    }
                }
            }
        }
        Card {
            icon: "edit-undo"
            title: qsTr("Reset")
            description: qsTr("Every app uses the default output and input again")
            trailing: Button {
                text: qsTr("Reset")
                usable: Audio.apps.some(a => a.output !== "" || a.input !== "")
                onClicked: Audio.resetApps()
            }
        }
    }
}
