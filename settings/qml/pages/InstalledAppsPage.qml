// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Apps > Installed apps: search and uninstall, like Windows 11.
Page {
    id: page
    Component.onDestruction: Apps.query = ""

    Item {
        width: parent.width
        height: 44
        TextBox {
            id: filter
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(360, parent.width)
            placeholderText: qsTr("Search apps")
            onTextChanged: Apps.query = text
        }
        Text {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            text: Apps.count + (Apps.count === 1 ? qsTr(" app found") : qsTr(" apps found"))
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }
    }

    Repeater {
        model: Apps
        delegate: Card {
            id: app
            required property string appId
            required property string name
            required property string iconName
            required property string comment
            icon: iconName
            title: name
            description: comment
            trailing: Button {
                subtle: true
                text: qsTr("Uninstall")
                usable: System.canUninstall(Apps.desktopFile(app.appId))
                onClicked: {
                    uninstallDialog.appId = app.appId
                    uninstallDialog.appName = app.name
                    uninstallDialog.open()
                }
            }
        }
    }

    Dialog {
        id: uninstallDialog
        parent: root.contentItem
        property string appId
        property string appName
        title: qsTr("Uninstall ") + appName
        buttons: [{ text: qsTr("Uninstall"), accent: true, value: "ok" }, { text: qsTr("Cancel"), value: "cancel" }]
        onChosen: value => { if (value === "ok") System.uninstall(Apps.desktopFile(appId)) }
        Text {
            width: parent.width
            text: qsTr("This app and its related info will be uninstalled. You may be asked for your password.")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
    }
}
