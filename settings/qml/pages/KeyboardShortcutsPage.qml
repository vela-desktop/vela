// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Dialogs
import QtCore
import Vela.Controls

// Keyboard > Custom shortcuts (docs/keyboard.md §9.1): the user's own
// combinations, each opening an app, running a command or opening a link or
// file. The combination is recorded with the compositor's own matcher
// (Shortcuts, settings/src/customshortcuts.h): Vela's combinations are
// refused, those without Win warned about, since apps use them.
Page {
    id: page

    CardGroup {
        Card {
            objectName: "shortcutsIntro"
            icon: "preferences-desktop-keyboard-shortcuts"
            title: Shortcuts.items.length > 0 ? qsTr("Your shortcuts") : qsTr("No custom shortcuts yet")
            description: Shortcuts.full ? qsTr("You have as many shortcuts as Vela can run. Remove one to add another.")
                : qsTr("Open an app, run a command, or open a link or file with a key combination")
            trailing: Button {
                objectName: "addShortcut"
                enabled: !Shortcuts.full
                text: qsTr("Add a shortcut")
                onClicked: editor.openFor(-1)
            }
        }
        Repeater {
            model: Shortcuts.items
            delegate: Card {
                id: shortcutCard
                required property var modelData
                required property int index
                objectName: "shortcut:" + modelData.combo
                icon: modelData.icon
                title: modelData.label
                description: modelData.kind === "app" ? qsTr("Opens %1").arg(modelData.name)
                    : modelData.kind === "command" ? qsTr("Runs %1").arg(modelData.target)
                    : qsTr("Opens %1").arg(modelData.target)
                trailing: Row {
                    spacing: 4
                    Button {
                        subtle: true
                        text: qsTr("Edit")
                        onClicked: editor.openFor(shortcutCard.index)
                    }
                    Button {
                        objectName: "remove:" + shortcutCard.modelData.combo
                        subtle: true
                        text: qsTr("Remove")
                        onClicked: if (!Shortcuts.remove(shortcutCard.index)) saveError.open()
                    }
                }
            }
        }
    }

    // What it does above, the combination below.
    Dialog {
        id: editor
        objectName: "shortcutDialog"
        parent: root.contentItem
        dialogWidth: 560
        property int index: -1
        property string kind: "app"
        property string appId: ""
        property string combo: ""
        // The last key recorded: {state, combo, label, reason, warning,
        // existing}.
        property var recorded: null
        readonly property string target: kind === "app" ? appId : targetField.text.trim()
        readonly property string problem: Shortcuts.targetProblem(kind, target)
        readonly property bool warning: recorded !== null && recorded.warning === true
        readonly property bool replaces: recorded !== null && recorded.existing >= 0 && recorded.existing !== index
        title: index < 0 ? qsTr("Add a shortcut") : qsTr("Edit the shortcut")
        buttons: [{ text: qsTr("Save"), accent: true, value: "save", enabled: combo !== "" && problem === "" },
            { text: qsTr("Cancel"), value: "cancel" }]
        onChosen: value => {
            recorder.stop()
            if (value !== "save") {
                return
            }
            if (warning) {
                confirm.open()
            } else {
                if (!Shortcuts.save(index, combo, kind, target)) saveError.open()
            }
        }
        onVisibleChanged: if (!visible) recorder.stop()

        function openFor(i) {
            index = i
            const item = i >= 0 ? Shortcuts.items[i] : null
            kind = item ? item.kind : "app"
            appId = item && item.kind === "app" ? item.target : ""
            targetField.text = item && item.kind !== "app" ? item.target : ""
            combo = item ? item.combo : ""
            recorded = item ? { state: "ok", combo: item.combo, label: item.label, warning: false, existing: i } : null
            appSearch.text = ""
            open()
        }

        Column {
            width: parent.width
            spacing: 8

            Text {
                text: qsTr("What it does")
                color: Theme.text
                font.pixelSize: Theme.fontBody
                font.weight: Theme.weightEmphasis
            }
            Row {
                spacing: 16
                RadioButton {
                    objectName: "kindApp"
                    text: qsTr("Open an app")
                    checked: editor.kind === "app"
                    onClicked: editor.kind = "app"
                }
                RadioButton {
                    objectName: "kindCommand"
                    text: qsTr("Run a command")
                    checked: editor.kind === "command"
                    onClicked: editor.kind = "command"
                }
                RadioButton {
                    objectName: "kindOpen"
                    text: qsTr("Open a link or file")
                    checked: editor.kind === "open"
                    onClicked: editor.kind = "open"
                }
            }

            // An app: a search and the list of installed apps.
            TextBox {
                id: appSearch
                objectName: "appSearch"
                visible: editor.kind === "app"
                width: parent.width
                placeholderText: qsTr("Search apps")
            }
            Rectangle {
                visible: editor.kind === "app"
                width: parent.width
                height: 200
                radius: Theme.radius
                color: "transparent"
                border.width: 1
                border.color: Theme.stroke
                ListView {
                    id: appList
                    objectName: "appList"
                    anchors { fill: parent; margins: 2 }
                    clip: true
                    model: editor.visible && editor.kind === "app" ? Shortcuts.apps(appSearch.text) : []
                    delegate: ListItem {
                        id: appRow
                        required property var modelData
                        objectName: "app:" + modelData.id
                        width: ListView.view.width
                        height: 36
                        highlighted: editor.appId === modelData.id
                        onClicked: editor.appId = modelData.id
                        Row {
                            anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                            spacing: 10
                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                name: appRow.modelData.icon
                                size: 20
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: appRow.modelData.name
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                            }
                        }
                    }
                }
            }

            // A command, or a link or file.
            Row {
                visible: editor.kind !== "app"
                width: parent.width
                spacing: 8
                TextBox {
                    id: targetField
                    objectName: "targetField"
                    width: parent.width - (browse.visible ? browse.width + parent.spacing : 0)
                    placeholderText: editor.kind === "command" ? qsTr("Such as kcalc, or notify-send Hello")
                        : qsTr("Such as https://example.org or a file")
                }
                Button {
                    id: browse
                    visible: editor.kind === "open"
                    text: qsTr("Browse")
                    onClicked: fileDialog.open()
                }
            }
            Text {
                objectName: "targetProblem"
                visible: editor.kind !== "app" && targetField.text !== "" && editor.problem !== ""
                width: parent.width
                text: editor.problem
                color: Theme.critical
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            Item { width: 1; height: 8 }
            Text {
                text: qsTr("The combination")
                color: Theme.text
                font.pixelSize: Theme.fontBody
                font.weight: Theme.weightEmphasis
            }
            Row {
                width: parent.width
                spacing: 12
                Button {
                    objectName: "record"
                    text: recorder.active ? qsTr("Press the keys…") : editor.combo !== "" ? qsTr("Record again") : qsTr("Record")
                    accent: recorder.active
                    onClicked: recorder.active ? recorder.stop() : recorder.start()
                }
                Text {
                    objectName: "comboLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    text: editor.recorded ? editor.recorded.label : qsTr("Not set")
                    color: editor.recorded && editor.recorded.state === "refused" ? Theme.textSecondary : Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Theme.weightEmphasis
                    font.strikeout: editor.recorded !== null && editor.recorded.state === "refused"
                }
            }
            Text {
                objectName: "comboRefused"
                visible: editor.recorded !== null && editor.recorded.state === "refused"
                width: parent.width
                text: editor.recorded ? editor.recorded.reason || "" : ""
                color: Theme.critical
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            Text {
                objectName: "comboReplaces"
                visible: editor.replaces
                width: parent.width
                text: qsTr("Another of your shortcuts has this combination: saving replaces it.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            // Apps use the combinations without Win: the warning stays
            // while one is shown.
            Rectangle {
                objectName: "comboWarning"
                visible: editor.warning
                width: parent.width
                height: warningText.implicitHeight + 20
                radius: Theme.radius
                color: Qt.rgba(Theme.caution.r, Theme.caution.g, Theme.caution.b, 0.14)
                border.width: 1
                border.color: Theme.caution
                Text {
                    id: warningText
                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 10 }
                    text: qsTr("Apps use combinations without Win, such as Ctrl+Alt+T or Ctrl+Shift+Esc. This one will reach no app any more, in any window.")
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }

            // Takes the keys while recording: Win and Vela's combinations
            // reach it, since Settings inhibits the compositor's shortcuts.
            Item {
                id: recorder
                objectName: "recorder"
                width: 1
                height: 1
                property bool active: false
                function start() {
                    active = true
                    forceActiveFocus()
                    Shortcuts.startRecording(page.Window.window)
                }
                function stop() {
                    if (!active) {
                        return
                    }
                    active = false
                    Shortcuts.stopRecording()
                }
                // Only while recording: afterwards the keys go up to the
                // dialog (Enter saves) instead of recording a plain Enter.
                Keys.enabled: active
                Keys.onPressed: event => {
                    event.accepted = true
                    if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) {
                        stop()
                        return
                    }
                    const result = Shortcuts.press(event.nativeScanCode, event.modifiers)
                    if (result.state === "modifier") {
                        return
                    }
                    editor.recorded = result
                    editor.combo = result.state === "ok" ? result.combo : ""
                    stop()
                }
                Keys.onReleased: event => { event.accepted = true }
                onActiveFocusChanged: if (!activeFocus) stop()
            }
        }
    }

    Dialog {
        id: confirm
        objectName: "shortcutConfirm"
        parent: root.contentItem
        title: qsTr("Take %1 from every app?").arg(editor.recorded ? editor.recorded.label : "")
        text: qsTr("Without Win, apps may use this combination. With this shortcut it opens what you chose, and no app gets it.")
        buttons: [{ text: qsTr("Save"), accent: true, value: "save" }, { text: qsTr("Back"), value: "back" }]
        onChosen: value => {
            if (value === "save") {
                if (!Shortcuts.save(editor.index, editor.combo, editor.kind, editor.target)) saveError.open()
            } else {
                editor.open()
            }
        }
    }

    Dialog {
        id: saveError
        parent: root.contentItem
        title: qsTr("Shortcut changes could not be saved")
        text: qsTr("Check the current shortcuts and try again. The file may have changed in another editor.")
        buttons: [{ text: qsTr("Close"), value: "close" }]
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Choose a file")
        currentFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)
        onAccepted: targetField.text = decodeURIComponent(selectedFile.toString().replace(/^file:\/\//, ""))
    }
}
