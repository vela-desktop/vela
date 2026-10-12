// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import Vela.Audio
import Vela.Controls

// System > Sound > an output's or an input's properties, like Windows 11:
// default device, its name, the device's profile (on a Bluetooth headset
// also its codec), turning it off, volume, balance, the test sound or the
// input's level, and its format (docs/audio.md §6.2, §8.1). A device turned
// off has no outputs or inputs: then only the device is shown, to turn it
// back on.
Page {
    id: page

    readonly property var selected: root.soundDevice
    readonly property bool output: selected.kind === "output"
    readonly property var endpoint: selected.kind === "" ? undefined
        : (output ? Audio.outputs : Audio.inputs).find(e => e.name === selected.name)
    readonly property string deviceName: endpoint !== undefined && endpoint.device !== "" ? endpoint.device : selected.device
    readonly property var device: Audio.devices.find(d => d.name === deviceName)
    readonly property var offProfile: device ? device.profiles.find(p => p.name === "off") : undefined
    readonly property bool playing: endpoint !== undefined && Audio.testSoundOutput === endpoint.name
    // WirePlumber names what its ALSA and Bluetooth monitors make; virtual
    // devices aren't theirs (docs/audio.md §8).
    readonly property bool renamable: endpoint !== undefined
        && (endpoint.canFormat || (device !== undefined && device.bluetooth))

    // The best profile to turn a device back on with.
    function bestProfile() {
        let best = undefined
        for (const p of device.profiles) {
            if (p.name !== "off" && p.available && (best === undefined || p.priority > best.priority)) {
                best = p
            }
        }
        return best
    }

    Component.onDestruction: if (playing) Audio.stopTestSound()

    LevelMeter {
        id: inputMeter
        audio: Audio
        kind: "input"
        name: page.endpoint !== undefined && !page.output ? page.endpoint.name : ""
        active: page.visible && root.visibility !== Window.Minimized && root.visibility !== Window.Hidden
    }

    Card {
        visible: page.endpoint !== undefined
        icon: page.endpoint ? page.endpoint.icon : ""
        title: page.endpoint ? page.endpoint.description : ""
        description: page.endpoint && page.endpoint.isDefault
            ? (page.output ? qsTr("Default output device") : qsTr("Default input device"))
            : ""
        minimumHeight: 80
        trailing: Row {
            spacing: 8
            Button {
                visible: page.renamable
                text: qsTr("Rename")
                onClicked: {
                    nameField.text = page.endpoint.description
                    renameDialog.open()
                    nameField.selectAll()
                    nameField.forceActiveFocus()
                }
            }
            Button {
                visible: page.endpoint !== undefined && !page.endpoint.isDefault
                text: qsTr("Set as default")
                usable: page.endpoint !== undefined && !page.endpoint.defaultPending
                onClicked: Audio.setDefault(page.selected.kind, page.endpoint.name)
            }
        }
    }

    // Unplugged, or its device turned off.
    Card {
        visible: page.endpoint === undefined
        icon: page.device ? page.device.icon : "dialog-information"
        title: page.device ? page.device.description : qsTr("This device isn't available")
        description: page.device && page.device.off
            ? qsTr("Turned off. Turn it on to use it again.")
            : qsTr("It may have been unplugged or turned off.")
        minimumHeight: 80
    }

    CardGroup {
        visible: page.device !== undefined
        title: page.device && page.device.bluetooth ? qsTr("Headset") : qsTr("Sound card")
        Card {
            visible: page.offProfile !== undefined
            icon: "system-shutdown"
            title: qsTr("Use this device")
            description: page.device && page.device.profileError ? qsTr("Couldn't change it. The device may be busy.") : ""
            trailing: Toggle {
                checked: page.device !== undefined && !page.device.off
                usable: page.device !== undefined && !page.device.profilePending
                    && (page.device.off ? page.bestProfile() !== undefined : true)
                onToggled: on => {
                    const p = on ? page.bestProfile() : page.offProfile
                    if (p !== undefined) {
                        Audio.setProfile(page.device.name, p.index)
                    }
                }
            }
        }
        Card {
            id: profileCard
            // Off is the switch above.
            readonly property var choices: page.device ? page.device.profiles.filter(p => p.name !== "off") : []
            visible: page.device !== undefined && !page.device.off && choices.length > 1
            icon: page.device && page.device.bluetooth ? "audio-headset" : "audio-card"
            title: page.device && page.device.bluetooth ? qsTr("Profile and codec") : qsTr("Profile")
            description: page.device && page.device.bluetooth
                ? qsTr("High quality for listening, or calls with the headset's microphone")
                : qsTr("Which outputs and inputs the sound card offers")
            trailing: Choice {
                model: profileCard.choices.map(p => ({ text: p.description }))
                usable: page.device !== undefined && !page.device.profilePending
                currentIndex: page.device ? profileCard.choices.findIndex(p => p.index === page.device.profile) : -1
                onChosen: index => Audio.setProfile(page.device.name, profileCard.choices[index].index)
            }
        }
    }

    CardGroup {
        visible: page.endpoint !== undefined
        title: page.output ? qsTr("Output settings") : qsTr("Input settings")
        Card {
            id: volumeCard
            icon: page.endpoint && page.endpoint.muted ? (page.output ? "audio-volume-muted" : "microphone-sensitivity-muted")
                : page.output ? "audio-volume-high" : "audio-input-microphone"
            title: qsTr("Volume")
            trailing: Row {
                spacing: 8
                Button {
                    subtle: true
                    icon: volumeCard.icon
                    Accessible.name: page.endpoint && page.endpoint.muted ? qsTr("Unmute") : qsTr("Mute")
                    usable: page.endpoint !== undefined && !page.endpoint.mutePending
                    onClicked: Audio.setMuted(page.selected.kind, page.endpoint.name, !page.endpoint.muted)
                }
                Slider {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 220
                    to: 1
                    stepSize: page.endpoint ? 1 / Math.min(50, page.endpoint.intervals) : 0.02
                    value: page.endpoint ? page.endpoint.volume : 0
                    onMoved: value => Audio.setVolume(page.selected.kind, page.endpoint.name, value)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32
                    text: page.endpoint ? Math.round(page.endpoint.volume * 100) : ""
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    horizontalAlignment: Text.AlignRight
                }
            }
        }
        Card {
            visible: page.endpoint !== undefined && page.endpoint.stereo
            icon: "audio-speakers"
            title: qsTr("Balance")
            description: page.endpoint && page.endpoint.volume <= 0 ? qsTr("Turn the volume up to change the balance.") : ""
            trailing: Row {
                spacing: 8
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Left")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                }
                Slider {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 220
                    enabled: page.endpoint !== undefined && page.endpoint.volume > 0
                    from: -1
                    to: 1
                    origin: 0
                    resetValue: 0
                    stepSize: 0.02
                    value: page.endpoint ? page.endpoint.balance : 0
                    // The middle holds the knob a little, so even is easy
                    // to find again; a double click puts it there.
                    onMoved: value => Audio.setBalance(page.selected.kind, page.endpoint.name, Math.abs(value) < 0.05 ? 0 : value)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Right")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                }
            }
        }
        Card {
            visible: page.output
            icon: "media-playback-start"
            title: qsTr("Test sound")
            description: qsTr("A chime on the left, then on the right")
            trailing: Button {
                text: page.playing ? qsTr("Stop") : qsTr("Test")
                onClicked: page.playing ? Audio.stopTestSound() : Audio.playTestSound(page.endpoint.name)
            }
        }
        Card {
            visible: !page.output
            icon: "audio-input-microphone"
            title: qsTr("Input level")
            description: page.endpoint && page.endpoint.muted
                ? qsTr("The microphone is muted")
                : qsTr("Speak to test your microphone")
            trailing: LevelBar {
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                width: 220 + 8 + 32 + 8 + 32
                level: inputMeter.level
            }
        }
    }

    // What it runs at, and what it may run at (docs/audio.md §8.1).
    CardGroup {
        id: formatGroup
        readonly property var endpoint: page.endpoint
        readonly property var rateChoices: endpoint ? [{ text: qsTr("Follow the system"), rate: 0 }]
            .concat(endpoint.rates.map(r => ({ text: qsTr("Always %1 Hz").arg(r), rate: r }))) : []
        readonly property var formatChoices: endpoint ? [{ text: qsTr("Best"), name: "" }]
            .concat(endpoint.formats.map(f => ({ text: qsTr("%1 bit").arg(f.bits), name: f.name }))) : []
        visible: endpoint !== undefined && endpoint.canFormat
        title: qsTr("Format")
        Text {
            width: parent.width
            text: qsTr("Changing the sample rate or format stops sound for about a second.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            bottomPadding: 4
        }
        Card {
            icon: "audio-x-generic"
            title: qsTr("Now")
            description: {
                const e = formatGroup.endpoint
                if (!e || e.rate <= 0) {
                    return page.output ? qsTr("Not playing") : qsTr("Not recording")
                }
                const now = e.bits > 0 ? qsTr("%1 Hz, %2 bit").arg(e.rate).arg(e.bits) : qsTr("%1 Hz").arg(e.rate)
                if (e.converted.length === 0) {
                    return now + " · " + qsTr("No conversion")
                }
                const apps = e.converted.map(c => qsTr("%1 (%2 Hz)").arg(c.name).arg(c.rate)).join(", ")
                return now + " · " + qsTr("Converted: %1").arg(apps)
            }
        }
        Card {
            icon: "chronometer"
            title: qsTr("Sample rate")
            description: qsTr("Following the system, it changes with what plays")
            trailing: Choice {
                model: formatGroup.rateChoices
                currentIndex: formatGroup.endpoint
                    ? Math.max(0, formatGroup.rateChoices.findIndex(c => c.rate === formatGroup.endpoint.fixedRate)) : -1
                onChosen: index => Audio.setEndpointRate(page.selected.kind, page.endpoint.name, formatGroup.rateChoices[index].rate)
            }
        }
        Card {
            visible: formatGroup.formatChoices.length > 2
            icon: "audio-card"
            title: qsTr("Sample format")
            description: qsTr("Choose another one only if the device has trouble with the best")
            trailing: Choice {
                model: formatGroup.formatChoices
                currentIndex: formatGroup.endpoint
                    ? Math.max(0, formatGroup.formatChoices.findIndex(c => c.name === formatGroup.endpoint.fixedFormat)) : -1
                onChosen: index => Audio.setEndpointFormat(page.selected.kind, page.endpoint.name, formatGroup.formatChoices[index].name)
            }
        }
    }

    Dialog {
        id: renameDialog
        parent: root.contentItem
        title: qsTr("Rename device")
        primaryText: qsTr("Rename")
        onAccepted: Audio.setEndpointName(page.selected.kind, page.endpoint.name, nameField.text)
        Column {
            width: parent.width
            spacing: 12
            Text {
                width: parent.width
                text: qsTr("Leave it empty to use the device's own name. Other apps show the new name after you sign in again.")
                color: Theme.text
                font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
            }
            TextBox {
                id: nameField
                width: parent.width
                Keys.onReturnPressed: { renameDialog.close(); renameDialog.accepted() }
            }
        }
    }
}
