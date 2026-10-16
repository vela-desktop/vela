// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Properties window of a file, a folder or several items, like in Windows:
// General (name, type, opens with, location, size, dates, attributes),
// Permissions (in place of "Security": read, write, execute for owner, group
// and others) and Details. OK applies and closes, Apply applies and stays,
// Cancel closes.
Window {
    id: root
    visible: false
    // The dialog with room for its shadow around it (docs/visual-coherence.md
    // §6.9).
    readonly property real room: shadow.reach
    width: room + 440 + room
    height: room + 600 + room
    color: "transparent"

    property var paths: []
    property var info: ({})
    property int tab: 0
    // Changes not applied yet.
    property string newName: ""
    property bool readOnly: false
    property int permissions: 0
    readonly property bool single: paths.length === 1
    readonly property bool changed: (single && newName !== info.name)
        || readOnly !== info.readOnly
        || (single && permissions !== info.permissions)
    property string sizeText: ""
    property string diskText: ""
    property string contentText: ""
    property string defaultApp: ""
    property string defaultAppIcon: ""

    Connections {
        target: Menus
        function onPropertiesRequested(paths) {
            root.load(paths)
            root.tab = 0
            root.visible = true
            motion.open()
            panel.forceActiveFocus()
        }
    }
    // From Explorer too (vela-files), through the shell's socket.
    Connections {
        target: Shell
        function onPropertiesRequested(paths) { Menus.showProperties(paths) }
    }
    Connections {
        target: Properties
        function onSizeCounted(token, bytes, onDisk, files, folders) {
            if (token === root.info.token) {
                root.showSizes(bytes, onDisk, files, folders)
            }
        }
    }

    function load(list) {
        paths = list
        info = Properties.describe(list)
        newName = info.name
        readOnly = info.readOnly
        permissions = info.permissions || 0
        if (info.bytes !== undefined) {
            showSizes(info.bytes, info.onDisk, info.files, info.folders)
        } else {
            sizeText = qsTr("Calculating…")
            diskText = sizeText
            contentText = sizeText
        }
        defaultApp = ""
        defaultAppIcon = ""
        if (single && !info.isDir) {
            const apps = Apps.appsForFile(list[0])
            const preferred = apps.find(a => a.isDefault) || apps[0]
            if (preferred) {
                defaultApp = preferred.name
                defaultAppIcon = preferred.icon
            }
        }
    }

    function showSizes(bytes, onDisk, files, folders) {
        sizeText = Properties.formatSize(bytes)
        diskText = Properties.formatSize(onDisk)
        contentText = files + (files === 1 ? qsTr(" file, ") : qsTr(" files, ")) + folders + (folders === 1 ? qsTr(" folder") : qsTr(" folders"))
    }

    function apply() {
        if (readOnly !== info.readOnly) {
            Properties.setReadOnly(paths, readOnly)
        }
        if (single && permissions !== info.permissions) {
            Properties.setPermissions(paths[0], permissions)
        }
        if (single && newName !== info.name && Desktop.rename(paths[0], newName)) {
            const dir = paths[0].substring(0, paths[0].lastIndexOf("/") + 1)
            paths = [dir + newName]
        }
        load(paths)
    }

    // Where the window is (in the center of the output): for menus.
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        return Qt.point((Screen.width - root.width) / 2 + p.x, (Screen.height - root.height) / 2 + p.y)
    }

    function close() {
        motion.close()
    }

    onActiveChanged: {
        if (!active && motion.opened && !Menus.isOpen) {
            close()
        }
    }

    component Label: Text {
        color: Theme.textSecondary
        font.pixelSize: Theme.fontBody
        width: 120
    }
    component Value: Text {
        color: Theme.text
        font.pixelSize: Theme.fontBody
        width: 260
        elide: Text.ElideMiddle
    }
    component Line: Rectangle {
        width: 392
        height: 1
        color: Theme.stroke
    }
    // A click in the shadow's room closes it, like a click outside.
    ShadowRoom {
        panels: [panel]
        onOutside: root.close()
    }

    // It grows in from 1.05, like Windows 11's dialogs, and fades out.
    PanelMotion {
        id: motion
        x: root.room
        y: root.room
        width: 440
        height: 600
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
            anchors.fill: parent
            radius: Theme.radiusOverlay
            color: Theme.dialog
            border.width: 1
            border.color: Theme.stroke
            focus: true
            Keys.onEscapePressed: root.close()
            Keys.onReturnPressed: { root.apply(); root.close() }

            Text {
                x: 24
                y: 16
                text: qsTr("Properties - ") + (root.info.name || "")
                color: Theme.text
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                width: parent.width - 48
                elide: Text.ElideMiddle
            }

            // Tabs
            Row {
                x: 20
                y: 44
                spacing: 4
                Repeater {
                    model: root.single ? [qsTr("General"), qsTr("Permissions"), qsTr("Details")] : [qsTr("General")]
                    delegate: Item {
                        required property string modelData
                        required property int index
                        width: tabLabel.implicitWidth + 24
                        height: 34
                        Text {
                            id: tabLabel
                            anchors.centerIn: parent
                            text: parent.modelData
                            color: root.tab === parent.index ? Theme.text : Theme.textSecondary
                            font.pixelSize: Theme.fontBody
                            font.weight: root.tab === parent.index ? Font.DemiBold : Font.Normal
                        }
                        Rectangle {
                            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom }
                            width: 20
                            height: 3
                            radius: 1.5
                            color: Theme.accent
                            visible: root.tab === parent.index
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.tab = parent.index
                        }
                    }
                }
            }

            // --- General ---
            Column {
                visible: root.tab === 0
                x: 24
                y: 92
                spacing: 12

                Row {
                    spacing: 16
                    Image {
                        width: 40
                        height: 40
                        source: "image://fileicon/" + encodeURIComponent(root.info.icon || "unknown")
                        sourceSize: Qt.size(width, height)
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 336
                        height: 32
                        radius: Theme.radius
                        color: Theme.control
                        border.width: 1
                        border.color: nameField.activeFocus ? Theme.accent : Theme.stroke
                        MenuTextField {
                            id: nameField
                            anchors { left: parent.left; right: parent.right; leftMargin: 10; rightMargin: 10; verticalCenter: parent.verticalCenter }
                            text: root.newName
                            readOnly: !root.single
                            mapToScreen: (x, y) => root.screenPoint(nameField, x, y)
                            onTextEdited: root.newName = text
                        }
                    }
                }
                Line {}
                Row { Label { text: qsTr("Type:") } Value { text: root.info.type || "" } }
                Row {
                    visible: root.defaultApp !== ""
                    height: 28
                    Label { text: qsTr("Opens with:"); anchors.verticalCenter: parent.verticalCenter }
                    Row {
                        width: 260
                        spacing: 8
                        anchors.verticalCenter: parent.verticalCenter
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: root.defaultAppIcon
                        }
                        Text {
                            width: 150
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.defaultApp
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                        }
                        Button {
                            id: changeButton
                            width: 80
                            implicitHeight: 28
                            text: qsTr("Change…")
                            font.pixelSize: Theme.fontCaption
                            onClicked: {
                                const path = root.paths[0]
                                const p = root.screenPoint(changeButton, 0, changeButton.height + 2)
                                Menus.open(Apps.appsForFile(path).map(a => ({
                                    text: a.name.replace(/&/g, "&&"), icon: a.icon, radio: true, checked: a.isDefault,
                                    action: () => { Properties.setDefaultApp(path, a.id); root.defaultApp = a.name; root.defaultAppIcon = a.icon }
                                })), p.x, p.y)
                            }
                        }
                    }
                }
                Line {}
                Row { Label { text: qsTr("Location:") } Value { text: root.info.location || "" } }
                Row { Label { text: qsTr("Size:") } Value { text: root.sizeText } }
                Row { Label { text: qsTr("Size on disk:") } Value { text: root.diskText } }
                Row {
                    visible: !!root.info.hasFolders || root.paths.length > 1
                    Label { text: qsTr("Contains:") }
                    Value { text: root.contentText }
                }
                Line { visible: root.single }
                Row { visible: root.single && !!root.info.created; Label { text: qsTr("Created:") } Value { text: root.info.created || "" } }
                Row { visible: root.single; Label { text: qsTr("Modified:") } Value { text: root.info.modified || "" } }
                Row { visible: root.single; Label { text: qsTr("Accessed:") } Value { text: root.info.accessed || "" } }
                Line {}
                Row {
                    spacing: 16
                    Label { text: qsTr("Attributes:"); width: 104; anchors.verticalCenter: parent.verticalCenter }
                    CheckBox {
                        text: qsTr("Read-only")
                        checked: root.readOnly
                        onToggled: root.readOnly = !root.readOnly
                    }
                    // On Linux "hidden" is the dot at the start of the name.
                    CheckBox {
                        text: qsTr("Hidden")
                        checked: !!root.info.hidden
                        usable: false
                    }
                }
                // A program or script starts with a double click only if it's
                // executable: the same "Execute" bit as in Permissions, for those
                // who can already read it.
                Row {
                    visible: root.single && !root.info.isDir
                    spacing: 16
                    Label { text: ""; width: 104 }
                    CheckBox {
                        text: qsTr("Allow executing as a program")
                        usable: !!root.info.mine
                        checked: (root.permissions & 0o111) !== 0
                        onToggled: {
                            if ((root.permissions & 0o111) !== 0)
                                root.permissions = root.permissions & ~0o111
                            else
                                root.permissions = root.permissions | ((root.permissions & 0o444) >> 2) | 0o100
                        }
                    }
                }
            }

            // --- Permissions ---
            Column {
                visible: root.tab === 1
                x: 24
                y: 92
                spacing: 14

                Row { Label { text: qsTr("Owner:") } Value { text: root.info.owner || "" } }
                Row { Label { text: qsTr("Group:") } Value { text: root.info.group || "" } }
                Line {}
                Text {
                    text: root.info.mine ? qsTr("Who can do what:") : qsTr("Only the owner can change the permissions.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }
                Repeater {
                    model: [{ who: qsTr("Owner"), shift: 6 }, { who: qsTr("Group"), shift: 3 }, { who: qsTr("Others"), shift: 0 }]
                    delegate: Row {
                        required property var modelData
                        spacing: 16
                        Label { text: modelData.who; width: 104 }
                        Repeater {
                            model: [{ what: qsTr("Read"), bit: 4 }, { what: qsTr("Write"), bit: 2 }, { what: root.info.isDir ? qsTr("Open") : qsTr("Execute"), bit: 1 }]
                            delegate: CheckBox {
                                required property var modelData
                                readonly property int mask: modelData.bit << parent.modelData.shift
                                text: modelData.what
                                usable: !!root.info.mine
                                checked: (root.permissions & mask) !== 0
                                onToggled: root.permissions = root.permissions ^ mask
                            }
                        }
                    }
                }
            }

            // --- Details ---
            Column {
                visible: root.tab === 2
                x: 24
                y: 92
                spacing: 12
                Row { Label { text: qsTr("Name:") } Value { text: root.info.name || "" } }
                Row { Label { text: qsTr("MIME type:") } Value { text: root.info.mimeName || "" } }
                Row { visible: !!root.info.imageSize; Label { text: qsTr("Size:") } Value { text: (root.info.imageSize || "") + qsTr(" pixels") } }
                Row { Label { text: qsTr("File size:") } Value { text: root.sizeText } }
            }

            Row {
                anchors { right: parent.right; bottom: parent.bottom; margins: 20 }
                spacing: 8

                Button {
                    text: "OK"
                    accent: true
                    onClicked: { root.apply(); root.close() }
                }
                Button {
                    text: qsTr("Cancel")
                    onClicked: root.close()
                }
                Button {
                    text: qsTr("Apply")
                    usable: root.changed
                    onClicked: root.apply()
                }
            }
        }
    }
}
