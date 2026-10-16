// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Explorer's Home, like Windows 11: the Quick access folders as tiles, then
// Favorite files and recently used ones.
Flickable {
    id: page
    property var tab
    contentHeight: column.height + 32
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    readonly property var recent: Places.recentFiles(40)
    property var favorites: Places.favorites
    Connections {
        target: Places
        function onFavoritesChanged() { page.favorites = Places.favorites }
    }

    // A file row (Favorites and Recent): name, date, folder.
    component FileRow: Fill {
        id: row
        required property var modelData
        width: column.width
        height: 32
        radius: Theme.radius
        color: rowMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
        Image {
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            source: "image://fileicon/" + encodeURIComponent(row.modelData.icon)
            sourceSize: Qt.size(width, height)
        }
        Text {
            x: 34
            width: parent.width * 0.38
            anchors.verticalCenter: parent.verticalCenter
            text: row.modelData.name
            color: Theme.text
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
        Text {
            x: parent.width * 0.42
            width: 150
            anchors.verticalCenter: parent.verticalCenter
            text: Qt.formatDateTime(row.modelData.modified, "dd/MM/yyyy HH:mm")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption + 1
        }
        Text {
            x: parent.width * 0.42 + 160
            width: parent.width - x - 8
            anchors.verticalCenter: parent.verticalCenter
            text: row.modelData.location
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption + 1
            elide: Text.ElideMiddle
        }
        MouseArea {
            id: rowMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onDoubleClicked: Ops.open([row.modelData.path])
            onClicked: mouse => {
                if (mouse.button === Qt.RightButton) {
                    const p = mapToItem(null, mouse.x, mouse.y)
                    const path = row.modelData.path
                    const favorite = Places.isFavorite(path)
                    page.tab.menus.open([
                        { text: qsTr("&Open"), icon: "document-open", action: () => Ops.open([path]) },
                        { text: qsTr("Open file &location"), icon: "folder-open", action: () => page.tab.navigate(row.modelData.location, path) },
                        { separator: true },
                        favorite
                            ? { text: qsTr("Remove from &Favorites"), icon: "starred-symbolic", action: () => Places.setFavorite(path, false) }
                            : { text: qsTr("Add to &Favorites"), icon: "starred-symbolic", action: () => Places.setFavorite(path, true) },
                        { text: qsTr("&Share"), icon: "document-share", action: () => Ops.share([path]) },
                        { text: qsTr("Copy as &path"), icon: "edit-copy-path", action: () => Ops.copyAsPath([path]) },
                        { text: qsTr("P&roperties"), icon: "document-properties", action: () => Ops.showProperties([path]) }
                    ], p.x, p.y)
                }
            }
        }
    }

    Column {
        id: column
        x: 24
        y: 16
        width: page.width - 48
        spacing: 8

        Text {
            text: qsTr("Quick access")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
            bottomPadding: 4
        }
        Flow {
            width: parent.width
            spacing: 4
            Repeater {
                model: Places.quickAccess
                delegate: Fill {
                    id: tile
                    required property var modelData
                    width: 220
                    height: 64
                    radius: Theme.radius
                    color: tileMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                    Image {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 40
                        height: 40
                        source: "image://fileicon/" + encodeURIComponent(tile.modelData.icon)
                        sourceSize: Qt.size(width, height)
                    }
                    Column {
                        x: 60
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 66
                        Text {
                            width: parent.width
                            text: tile.modelData.name
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: qsTr("Pinned")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                    MouseArea {
                        id: tileMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                        onDoubleClicked: page.tab.navigate(tile.modelData.path)
                        onClicked: mouse => {
                            if (mouse.button === Qt.MiddleButton) {
                                page.tab.openInNewTab(tile.modelData.path)
                            } else if (mouse.button === Qt.RightButton) {
                                const p = mapToItem(null, mouse.x, mouse.y)
                                page.tab.menus.open([
                                    { text: qsTr("&Open"), icon: "document-open", action: () => page.tab.navigate(tile.modelData.path) },
                                    { text: qsTr("Open in new &tab"), icon: "tab-new", action: () => page.tab.openInNewTab(tile.modelData.path) },
                                    { separator: true },
                                    { text: qsTr("&Unpin from Quick access"), icon: "window-unpin", action: () => Places.unpin(tile.modelData.path) },
                                    { text: qsTr("P&roperties"), icon: "document-properties", action: () => Ops.showProperties([tile.modelData.path]) }
                                ], p.x, p.y)
                            }
                        }
                    }
                    DropArea {
                        anchors.fill: parent
                        onEntered: drag => { if (!drag.hasUrls) drag.accepted = false }
                        onDropped: drop => {
                            Ops.drop(drop.urls.map(u => u.toString()), tile.modelData.path, 0)
                            drop.accept(Qt.MoveAction)
                        }
                    }
                }
            }
        }

        Text {
            text: qsTr("Favorites")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
            topPadding: 20
            bottomPadding: 4
        }
        Text {
            visible: page.favorites.length === 0
            text: qsTr("After you've added some files to Favorites, we'll show them here.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }
        Repeater {
            model: page.favorites
            delegate: FileRow {}
        }

        Text {
            text: qsTr("Recent")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
            topPadding: 20
            bottomPadding: 4
        }
        Text {
            visible: page.recent.length === 0
            text: qsTr("Files you've opened recently will show up here.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }
        Repeater {
            model: page.recent
            delegate: FileRow {}
        }
    }
}
