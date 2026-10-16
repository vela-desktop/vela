// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Three file dialogs, for the desktop and for Explorer (which asks for
// them through the shell's socket), like Windows 11:
// - Share: to nearby devices (KDE Connect), by mail, via Bluetooth;
// - "Choose another app": suggested apps and all the others, "Just once" or
//   "Always" (it becomes the default app for that file type);
// - New > Shortcut: the two-step wizard (the item, then the name).
Window {
    id: root
    visible: false
    width: panel.width + 64
    height: panel.height + 64
    color: "transparent"

    property string mode: "" // "share", "openWith" or "shortcut"
    // Share
    property var paths: []
    // Choose another app
    property string path: ""
    property var suggested: []
    property var others: []
    property string chosen: ""
    // New shortcut
    property string folder: ""
    property int step: 0
    property string error: ""

    Connections {
        target: Menus
        function onShareRequested(paths) { root.openShare(paths) }
        function onOpenWithRequested(path) { root.openWith(path) }
        function onNewShortcutRequested(folder) { root.openShortcut(folder) }
    }
    // From Explorer too.
    Connections {
        target: Shell
        function onShareRequested(paths) { root.openShare(paths) }
        function onOpenWithRequested(path) { root.openWith(path) }
        function onNewShortcutRequested(folder) { root.openShortcut(folder) }
    }

    function show() {
        visible = true
        panel.opacity = 0
        panel.scale = 1 - 0.04 * Theme.motion
        appear.restart()
        panel.forceActiveFocus()
    }
    function close() {
        visible = false
        mode = ""
    }
    function openShare(list) {
        paths = list
        mode = "share"
        FileActions.findDevices()
        show()
    }
    function openWith(file) {
        path = file
        suggested = Apps.appsForFile(file)
        const ids = suggested.map(a => a.id)
        others = Apps.allApps().filter(a => ids.indexOf(a.id) < 0)
        chosen = suggested.length > 0 ? suggested[0].id : ""
        mode = "openWith"
        show()
    }
    function openShortcut(dir) {
        folder = dir
        step = 0
        error = ""
        targetField.text = ""
        nameField.text = ""
        mode = "shortcut"
        show()
        targetField.forceActiveFocus()
    }
    function launchChosen(always) {
        if (chosen === "") return
        if (always) FileActions.setDefaultApp(chosen, path)
        Apps.launchWithFile(chosen, "file://" + encodeURI(path).replace(/#/g, "%23").replace(/\?/g, "%3F"))
        close()
    }
    function shortcutNext() {
        if (step === 0) {
            if (targetField.text.trim() === "") return
            error = ""
            nameField.text = FileActions.shortcutName(targetField.text, folder)
            step = 1
            nameField.forceActiveFocus()
            nameField.selectAll()
            return
        }
        if (nameField.text.trim() === "") return
        const result = FileActions.createShortcut(folder, targetField.text, nameField.text)
        if (result !== "") {
            error = result
            step = 0
            targetField.forceActiveFocus()
            return
        }
        close()
    }

    // In the center of the output: from window to output coordinates (menus).
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        return Qt.point((Screen.width - root.width) / 2 + p.x, (Screen.height - root.height) / 2 + p.y)
    }

    onActiveChanged: {
        if (!active && visible && !Menus.isOpen) {
            close()
        }
    }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: panel; property: "opacity"; to: 1; duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        NumberAnimation { target: panel; property: "scale"; to: 1; duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }

    component Title: Text {
        width: parent.width
        color: Theme.text
        font.pixelSize: 20
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }
    component Section: Text {
        color: Theme.text
        font.pixelSize: Theme.fontBody
        font.weight: Font.DemiBold
        topPadding: 8
    }
    component Note: Text {
        width: parent.width
        color: Theme.textSecondary
        font.pixelSize: Theme.fontBody
        wrapMode: Text.WordWrap
    }
    // A clickable row with an icon (Share targets, apps).
    component Choice: Rectangle {
        id: choice
        property string icon
        property string label
        property bool selected: false
        signal clicked()
        signal doubleClicked()
        width: parent.width
        height: 40
        radius: Theme.radius
        color: selected ? Theme.subtleHover : choiceMouse.containsMouse ? Theme.subtleHover : "transparent"
        border.width: selected ? 1 : 0
        border.color: Theme.accent
        Image {
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 24
            height: 24
            source: Theme.icons + encodeURIComponent(choice.icon)
            sourceSize: Qt.size(width, height)
        }
        Text {
            x: 44
            width: parent.width - 52
            anchors.verticalCenter: parent.verticalCenter
            text: choice.label
            color: Theme.text
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
        MouseArea {
            id: choiceMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: choice.clicked()
            onDoubleClicked: choice.doubleClicked()
        }
    }
    // A text field whose border lights up.
    component Field: Rectangle {
        property alias input: field
        property alias text: field.text
        width: parent.width
        height: 32
        radius: Theme.radius
        color: Theme.control
        border.width: 1
        border.color: field.activeFocus ? Theme.accent : Theme.stroke
        MenuTextField {
            id: field
            anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
            verticalAlignment: TextInput.AlignVCenter
            mapToScreen: (x, y) => root.screenPoint(field, x, y)
            onAccepted: root.shortcutNext()
        }
    }

    Shadow {
        target: panel
        opacity: panel.opacity
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: root.mode === "openWith" ? 440 : root.mode === "shortcut" ? 500 : 420
        height: (root.mode === "share" ? share.height : root.mode === "openWith" ? openWith.height : shortcut.height) + 48
        radius: Theme.radiusOverlay
        color: Theme.dialog
        border.width: 1
        border.color: Theme.stroke
        focus: true
        Keys.onEscapePressed: root.close()

        // Close, at the top right.
        Rectangle {
            anchors { right: parent.right; top: parent.top; margins: 8 }
            width: 32
            height: 32
            radius: Theme.radius
            color: closeMouse.containsMouse ? "#c42b1c" : "transparent"
            Text {
                anchors.centerIn: parent
                text: "✕"
                color: closeMouse.containsMouse ? "white" : Theme.text
                font.pixelSize: 12
            }
            MouseArea {
                id: closeMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.close()
            }
        }

        // ---------------------------------------------------------- Share --
        Column {
            id: share
            visible: root.mode === "share"
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 8

            Title { text: qsTr("Share") }
            Row {
                spacing: 10
                Image {
                    width: 32
                    height: 32
                    source: root.paths.length === 1 ? "image://fileicon/" + encodeURIComponent(FileActions.iconFor(root.paths[0])) : Theme.icons + "document-multiple"
                    sourceSize: Qt.size(width, height)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: share.width - 42
                    text: root.paths.length === 1 ? root.paths[0].substring(root.paths[0].lastIndexOf("/") + 1) : root.paths.length + qsTr(" items")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    elide: Text.ElideMiddle
                }
            }

            Section { text: qsTr("Nearby sharing") }
            Note {
                visible: !FileActions.canKdeConnect()
                text: qsTr("To send files to your phone or another computer, install KDE Connect and pair them.")
            }
            Note {
                visible: FileActions.canKdeConnect() && FileActions.devices.length === 0
                text: FileActions.searching ? qsTr("Looking for devices...")
                    : qsTr("No devices nearby. Pair your phone or computer with KDE Connect.")
            }
            Flow {
                width: parent.width
                spacing: 8
                Repeater {
                    model: FileActions.devices
                    delegate: Rectangle {
                        id: device
                        required property var modelData
                        width: 112
                        height: 92
                        radius: Theme.radius
                        color: deviceMouse.containsMouse ? Theme.subtleHover : Theme.control
                        border.width: 1
                        border.color: Theme.stroke
                        Image {
                            anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 14 }
                            width: 32
                            height: 32
                            source: Theme.icons + "smartphone"
                            sourceSize: Qt.size(width, height)
                        }
                        Text {
                            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 12 }
                            width: parent.width - 12
                            text: device.modelData.name
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            id: deviceMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                FileActions.sendToDevice(device.modelData.id, root.paths)
                                root.close()
                            }
                        }
                    }
                }
            }

            Section { text: qsTr("Share using") }
            Choice {
                visible: FileActions.canEmail()
                icon: "mail-message-new"
                label: qsTr("Email")
                onClicked: { FileActions.sendByEmail(root.paths); root.close() }
            }
            Choice {
                visible: FileActions.canBluetooth() && Status.bluetoothAvailable
                icon: "preferences-system-bluetooth"
                label: qsTr("Bluetooth")
                onClicked: { FileActions.sendByBluetooth(root.paths); root.close() }
            }
            Note {
                visible: !FileActions.canEmail() && !(FileActions.canBluetooth() && Status.bluetoothAvailable)
                text: qsTr("No apps to share with.")
            }
        }

        // -------------------------------------------- Choose another app --
        Column {
            id: openWith
            visible: root.mode === "openWith"
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 8

            Title {
                width: parent.width - 32
                text: qsTr("Select an app to open this file") + (FileActions.extension(root.path) !== "" ? " " + FileActions.extension(root.path) : "")
            }
            Note { text: FileActions.typeName(root.path) }

            Flickable {
                width: parent.width
                height: 340
                contentHeight: appList.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Column {
                    id: appList
                    width: parent.width
                    spacing: 2
                    Section { visible: root.suggested.length > 0; text: qsTr("Suggested apps"); topPadding: 0; bottomPadding: 4 }
                    Repeater {
                        model: root.suggested
                        delegate: Choice {
                            required property var modelData
                            icon: modelData.icon
                            label: modelData.name
                            selected: root.chosen === modelData.id
                            onClicked: root.chosen = modelData.id
                            onDoubleClicked: { root.chosen = modelData.id; root.launchChosen(false) }
                        }
                    }
                    Section { text: qsTr("Other apps"); bottomPadding: 4 }
                    Repeater {
                        model: root.others
                        delegate: Choice {
                            required property var modelData
                            icon: modelData.icon
                            label: modelData.name
                            selected: root.chosen === modelData.id
                            onClicked: root.chosen = modelData.id
                            onDoubleClicked: { root.chosen = modelData.id; root.launchChosen(false) }
                        }
                    }
                }
            }
            Row {
                anchors.right: parent.right
                spacing: 8
                Button { text: qsTr("Always"); accent: true; usable: root.chosen !== ""; onClicked: root.launchChosen(true) }
                Button { text: qsTr("Just once"); usable: root.chosen !== ""; onClicked: root.launchChosen(false) }
            }
        }

        // ------------------------------------------------ New shortcut --
        Column {
            id: shortcut
            visible: root.mode === "shortcut"
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 10

            Title {
                width: parent.width - 32
                text: root.step === 0 ? qsTr("What item would you like to create a shortcut for?") : qsTr("What would you like to name the shortcut?")
            }
            Note {
                visible: root.step === 0
                text: qsTr("You can create shortcuts to files, folders and programs, on this computer or on a network, and to Internet addresses.")
            }
            Text {
                text: root.step === 0 ? qsTr("Type the location of the item:") : qsTr("Type a name for this shortcut:")
                color: Theme.text
                font.pixelSize: Theme.fontBody
            }
            Field { id: targetFieldBox; visible: root.step === 0 }
            Field { id: nameFieldBox; visible: root.step === 1 }
            Text {
                visible: root.error !== ""
                width: parent.width
                text: root.error
                color: Theme.light ? "#c42b1c" : "#ff99a4"
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WordWrap
            }
            Item { width: 1; height: 8 }
            Row {
                anchors.right: parent.right
                spacing: 8
                Button {
                    text: root.step === 0 ? qsTr("Next") : qsTr("Finish")
                    accent: true
                    usable: (root.step === 0 ? targetFieldBox.text : nameFieldBox.text).trim() !== ""
                    onClicked: root.shortcutNext()
                }
                Button {
                    text: root.step === 0 ? qsTr("Cancel") : qsTr("Back")
                    onClicked: {
                        if (root.step === 0) {
                            root.close()
                        } else {
                            root.step = 0
                            targetFieldBox.input.forceActiveFocus()
                        }
                    }
                }
            }
        }
    }

    // The wizard's two fields, by name.
    property alias targetField: targetFieldBox.input
    property alias nameField: nameFieldBox.input
}
