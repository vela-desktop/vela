// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Bluetooth
import Vela.Controls
import "bluetooth.js" as State

// "Add a device" (docs/bluetooth.md §9.1): a discoverable search while it
// is open, and the devices found, nearest first. A click starts an Add; while
// a device pairs the others wait. The Add's questions replace the list, and
// the dialog closes once the device is added. Cancel (or Esc) stops the Add.
// When the search ends the list stays, with "Search again".
Dialog {
    id: dialog
    // The device this dialog is adding, if any.
    property string adding: ""
    readonly property bool asking: Bluetooth.question.drawnBy === "settings"
    readonly property var pairingDevice: Bluetooth.devices.find(d => d.path === Bluetooth.pairing)

    title: qsTr("Add a device")
    dialogWidth: 520
    buttons: [{ text: qsTr("Cancel"), value: "cancel" }]

    function start() {
        adding = ""
        Bluetooth.clearError()
        open()
        Bluetooth.startSearch()
    }

    onChosen: {
        if (adding !== "" && Bluetooth.pairing === adding)
            Bluetooth.cancel(adding)
        adding = ""
        Bluetooth.stopSearch()
    }
    Connections {
        target: Bluetooth
        function onAdded(path) {
            if (path === dialog.adding)
                dialog.close("added")
        }
        // Each new question takes the focus, once it is shown.
        function onQuestionChanged() {
            if (dialog.asking && dialog.visible)
                Qt.callLater(question.takeFocus)
        }
    }

    Column {
        width: parent.width
        spacing: 8

        BluetoothQuestion {
            id: question
            objectName: "question"
            visible: dialog.asking
            // The dialog's Cancel ends the Add.
            declinable: false
            question: dialog.asking ? Bluetooth.question : ({})
            onAnswered: (response, value) => Bluetooth.answer(Bluetooth.question.id, response, value)
        }

        Text {
            visible: !dialog.asking
            width: parent.width
            text: qsTr("Make sure your device is turned on and discoverable. Select it below to add it.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        // What BlueZ says, never what was asked for (§6.6, rule 2).
        Text {
            objectName: "visibility"
            visible: !dialog.asking && text !== ""
            width: parent.width
            text: Bluetooth.adapter.discoverable ? qsTr("Discoverable as \"%1\"").arg(Bluetooth.adapter.name)
                : Bluetooth.searching && Bluetooth.adapter.discovering
                    ? qsTr("Another app is searching for devices, so this PC isn't made visible. Pairing from this PC still works.")
                    : ""
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        Text {
            objectName: "wait"
            visible: !dialog.asking && dialog.pairingDevice !== undefined
            width: parent.width
            text: dialog.pairingDevice ? qsTr("Wait for %1 to finish pairing.").arg(dialog.pairingDevice.name) : ""
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }

        Item {
            visible: !dialog.asking
            width: parent.width
            height: Math.min(320, Math.max(found.contentHeight, 80))
            ListView {
                id: found
                objectName: "found"
                anchors.fill: parent
                clip: true
                spacing: 2
                // The device being added stays here once paired, until it
                // is connected.
                model: Bluetooth.found.concat(Bluetooth.paired.filter(d => d.path === dialog.adding))
                boundsBehavior: Flickable.StopAtBounds
                delegate: ListItem {
                    id: candidate
                    required property var modelData
                    objectName: "device:" + modelData.name
                    width: found.width
                    height: 56
                    text: modelData.name
                    enabled: Bluetooth.pairing === "" && Bluetooth.active
                    opacity: enabled || Bluetooth.pairing === modelData.path ? 1 : 0.45
                    onClicked: {
                        dialog.adding = modelData.path
                        Bluetooth.add(modelData.path)
                    }
                    contentItem: Item {
                        Icon {
                            x: 4
                            anchors.verticalCenter: parent.verticalCenter
                            name: State.icon(candidate.modelData)
                            size: 24
                        }
                        Column {
                            x: 44
                            width: parent.width - x
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                width: parent.width
                                text: candidate.modelData.name
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                visible: text !== ""
                                text: State.state(candidate.modelData)
                                color: candidate.modelData.message && !candidate.modelData.operation ? Theme.critical : Theme.textSecondary
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
            Text {
                visible: found.count === 0
                anchors.centerIn: parent
                text: Bluetooth.searching ? qsTr("Looking for devices...") : qsTr("No devices found")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }
        }

        Button {
            objectName: "searchAgain"
            visible: !dialog.asking && Bluetooth.searchEnded
            text: qsTr("Search again")
            onClicked: Bluetooth.startSearch()
        }

        Text {
            objectName: "error"
            visible: text !== ""
            width: parent.width
            text: Bluetooth.error
            color: Theme.critical
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
    }
}
