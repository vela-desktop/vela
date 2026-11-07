// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Bluetooth
import Vela.Bluetooth.Ui
import Vela.Controls

// A question of the Bluetooth agent the shell draws (docs/bluetooth.md
// §9.3, bluetoothprompts.h): a small dialog in the center of the primary
// screen, like ConfirmDialog, with the device, the code in large digits and
// the field when one is needed (BluetoothQuestion). Enter gives the first
// answer, Esc refuses or cancels. It closes by itself when the question
// does, wherever it was answered; going to another window leaves it open,
// since the device is still waiting.
Window {
    id: root
    visible: false
    readonly property real room: shadow.reach
    width: room + 420 + room
    height: room + d.panelHeight + room
    color: "transparent"

    QtObject {
        id: d
        // The question shown: the last one while the dialog leaves.
        property var question: ({})
        readonly property real panelHeight: 24 + view.height + (error.visible ? error.height + 12 : 0) + 24
    }

    function show() {
        const question = BluetoothPrompts.dialog
        if (question.id === undefined) {
            motion.close()
            return
        }
        const opening = !motion.opened || d.question.id !== question.id
        d.question = question
        if (!opening)
            return
        Bluetooth.clearError()
        if (!motion.opened) {
            Shell.placeOnScreen(root, Shell.primaryScreen)
            root.visible = true
            motion.open()
        }
        view.takeFocus()
    }

    Connections {
        target: BluetoothPrompts
        function onDialogChanged() { root.show() }
    }

    // It grows in from 1.05, like Windows 11's dialogs, and fades out.
    PanelMotion {
        id: motion
        x: root.room
        y: root.room
        width: 420
        height: d.panelHeight
        scaleFrom: 1.05
        duration: Theme.normal
        onClosed: root.visible = false

        Shadow {
            id: shadow
            target: panel
            depth: Theme.shadow.dialog
        }

        Rectangle {
            id: panel
            objectName: "bluetoothDialog"
            anchors.fill: parent
            radius: Theme.radiusOverlay
            color: Theme.dialog
            border.width: 1
            border.color: Theme.stroke
            Accessible.role: Accessible.Dialog
            Accessible.name: qsTr("Bluetooth")
            Keys.onEscapePressed: view.decline()

            BluetoothQuestion {
                id: view
                x: 24
                y: 24
                width: parent.width - 48
                question: d.question
                onAnswered: (response, value) => BluetoothPrompts.answer(response, value)
            }
            // An answer refused (a code out of range, the screen locked...).
            Text {
                id: error
                objectName: "error"
                x: 24
                anchors { top: view.bottom; topMargin: 12 }
                width: parent.width - 48
                visible: Bluetooth.error !== ""
                text: Bluetooth.error
                textFormat: Text.PlainText
                color: Theme.critical
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }
    }
}
