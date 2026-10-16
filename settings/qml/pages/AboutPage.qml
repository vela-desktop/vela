// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// System > About: the device and the system, like Windows 11.
Page {
    id: page

    Item {
        width: parent.width
        height: 84
        Image {
            id: deviceIcon
            anchors.verticalCenter: parent.verticalCenter
            width: 64
            height: 64
            source: Theme.icons + About.chassisIcon
            sourceSize: Qt.size(width, height)
        }
        Column {
            anchors { left: deviceIcon.right; leftMargin: 16; verticalCenter: parent.verticalCenter }
            Text {
                text: About.hostname
                color: Theme.text
                font.pixelSize: Theme.fontSubtitle
                font.weight: Font.DemiBold
            }
            Text {
                text: About.model
                visible: text !== ""
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }
        }
        Button {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            text: qsTr("Rename this PC")
            onClicked: {
                nameField.text = About.hostname
                renameDialog.open()
                nameField.forceActiveFocus()
                nameField.selectAll()
            }
        }
    }

    component SpecGroup: Card {
        id: spec
        property var rows: []
        property string header
        icon: "help-about"
        title: header
        trailing: Button {
            text: qsTr("Copy")
            onClicked: {
                copyHelper.text = About.asText()
                copyHelper.selectAll()
                copyHelper.copy()
            }
        }
        contentItem: Column {
            width: parent.width
            spacing: 6
            Repeater {
                model: spec.rows
                delegate: Row {
                    required property var modelData
                    width: parent.width
                    Text {
                        width: 200
                        leftPadding: 36
                        text: modelData.label
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width - 200
                        text: modelData.value
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    // For "Copy": the text goes to the clipboard through a hidden field.
    TextEdit { id: copyHelper; visible: false }

    SpecGroup {
        header: qsTr("Device specifications")
        rows: About.device
    }
    SpecGroup {
        icon: "start-here"
        header: qsTr("System specifications")
        rows: About.system
    }

    CardGroup {
        title: qsTr("Related settings")
        LinkCard {
            visible: System.available("system")
            icon: "hwinfo"
            title: qsTr("Detailed system information")
            description: qsTr("Hardware, drivers, firmware")
            onClicked: System.trigger("system")
        }
        LinkCard {
            visible: System.available("task-manager")
            icon: "utilities-system-monitor"
            title: qsTr("Task manager")
            description: qsTr("Processes, performance, CPU and memory usage")
            onClicked: System.trigger("task-manager")
        }
    }

    Dialog {
        id: renameDialog
        parent: root.contentItem
        readonly property bool valid: /^[A-Za-z0-9][A-Za-z0-9-]{0,62}$/.test(nameField.text)
        title: qsTr("Rename your PC")
        buttons: [
            { text: qsTr("Next"), accent: true, value: "ok", enabled: renameDialog.valid },
            { text: qsTr("Cancel"), value: "cancel" }
        ]
        onChosen: value => { if (value === "ok") About.rename(nameField.text) }
        Column {
            width: parent.width
            spacing: 12
            Text {
                width: parent.width
                text: qsTr("Your PC's current name is ") + About.hostname + qsTr(". You can use letters, numbers and hyphens.")
                color: Theme.text
                font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
            }
            TextBox {
                id: nameField
                width: parent.width
                Keys.onReturnPressed: renameDialog.accept()
            }
        }
    }

    Connections {
        target: About
        function onRenameFailed(message) {
            errorText.text = message
            errorDialog.open()
        }
    }
    Dialog {
        id: errorDialog
        parent: root.contentItem
        title: qsTr("Couldn't rename the PC")
        buttons: [{ text: qsTr("Close"), value: "cancel" }]
        Text {
            id: errorText
            width: parent.width
            color: Theme.text
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
    }
}
