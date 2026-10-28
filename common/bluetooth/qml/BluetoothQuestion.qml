// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A question of the Bluetooth agent (docs/bluetooth.md §6.4, §9.3), inline
// in Settings' "Add a device" and in the shell's dialog: what the device
// asks, the code in large digits, a field when one is needed, and the
// answers. Enter gives the first answer; Esc is left to the dialog around
// it. The time left shows once fewer than 15 seconds remain.
FocusScope {
    id: view
    // Vela.Bluetooth's question: {id, kind, name, passkey, entered, pin,
    // service, deadline...}.
    property var question: ({})
    // Its own Cancel or Deny; a dialog with a Cancel of its own hides it.
    property bool declinable: true
    signal answered(string response, string value)

    readonly property string kind: question.kind || ""
    readonly property string name: question.name || ""
    readonly property bool typed: kind === "enter-passkey" || kind === "enter-pin"
    // The user types the code on the device: only Cancel here.
    readonly property bool shown: kind === "show-passkey" || kind === "show-pin"
    readonly property string code: kind === "show-pin" ? (question.pin || "") : (question.passkey || "")
    readonly property bool valid: kind === "enter-passkey" ? /^[0-9]{6}$/.test(field.text)
                                : kind === "enter-pin" ? field.text.length > 0 : !shown
    property real now: Date.now()
    readonly property int secondsLeft: question.deadline ? Math.max(0, Math.ceil((question.deadline - now) / 1000)) : -1

    width: parent ? parent.width : 0
    height: column.height

    // The field when there is one, otherwise the question itself.
    function takeFocus() {
        if (typed)
            field.forceActiveFocus()
        else
            view.forceActiveFocus()
    }
    function accept() {
        if (valid)
            answered("accept", typed ? field.text : "")
    }
    function decline() {
        // A code to confirm, or a request to allow, is refused; typing a
        // code is cancelled (§7.2).
        answered(kind === "confirm-passkey" || kind === "authorize-pairing" || kind === "authorize-service" ? "reject" : "cancel", "")
    }

    onQuestionChanged: field.text = ""
    Keys.onReturnPressed: accept()
    Keys.onEnterPressed: accept()

    Timer {
        interval: 1000
        repeat: true
        triggeredOnStart: true
        running: view.visible && view.question.deadline !== undefined
        onTriggered: view.now = Date.now()
    }

    Column {
        id: column
        width: parent.width
        spacing: 12

        Row {
            spacing: 12
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: view.question.icon && view.question.icon !== "bluetooth" ? view.question.icon : "preferences-system-bluetooth"
                size: 32
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: view.name
                color: Theme.text
                font.pixelSize: Theme.fontBodyLarge
                font.weight: Font.DemiBold
            }
        }
        Text {
            objectName: "prompt"
            width: parent.width
            text: {
                switch (view.kind) {
                case "confirm-passkey": return qsTr("Does %1 show this code?").arg(view.name)
                case "authorize-pairing": return qsTr("%1 wants to pair with this PC.").arg(view.name)
                case "authorize-service": return qsTr("Allow %1 to use %2?").arg(view.name).arg(view.question.service || "")
                case "enter-passkey": return qsTr("Type the 6-digit code shown on %1.").arg(view.name)
                case "enter-pin": return qsTr("Type the PIN for %1.").arg(view.name)
                case "show-passkey":
                case "show-pin": return qsTr("Type this code on %1, then press Enter on it.").arg(view.name)
                }
                return ""
            }
            color: Theme.text
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        Text {
            objectName: "code"
            visible: view.code !== ""
            text: view.code
            color: Theme.text
            font.pixelSize: 40
            font.weight: Font.DemiBold
            font.letterSpacing: 4
            // Read digit by digit.
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Code %1").arg(view.code.split("").join(" "))
        }
        Text {
            objectName: "entered"
            visible: view.kind === "show-passkey" && (view.question.entered || 0) > 0
            text: qsTr("%1 of 6 digits typed").arg(view.question.entered || 0)
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }
        TextBox {
            id: field
            objectName: "field"
            visible: view.typed
            width: Math.min(parent.width, 240)
            maximumLength: view.kind === "enter-passkey" ? 6 : 16
            validator: RegularExpressionValidator {
                regularExpression: view.kind === "enter-passkey" ? /[0-9]{0,6}/ : /[^\x00-\x1f\x7f]*/
            }
            inputMethodHints: view.kind === "enter-passkey" ? Qt.ImhDigitsOnly : Qt.ImhNoPredictiveText
            Accessible.name: view.kind === "enter-passkey" ? qsTr("Code") : qsTr("PIN")
        }
        Text {
            objectName: "timeLeft"
            visible: view.secondsLeft >= 0 && view.secondsLeft < 15
            text: qsTr("Time left: %1 s").arg(view.secondsLeft)
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
        }
        Row {
            spacing: 8
            Button {
                objectName: "accept"
                visible: !view.shown
                accent: true
                usable: view.valid
                text: view.kind === "authorize-pairing" || view.kind === "authorize-service" ? qsTr("Allow") : qsTr("Pair")
                onClicked: view.accept()
            }
            Button {
                objectName: "decline"
                visible: view.declinable
                text: view.kind === "authorize-pairing" || view.kind === "authorize-service" ? qsTr("Deny") : qsTr("Cancel")
                onClicked: view.decline()
            }
        }
    }
}
