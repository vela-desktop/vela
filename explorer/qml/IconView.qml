// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Windows 11 icon views: large icons (with thumbnails), medium and small
// (icon and name on one line). Same selection, dragging and renaming as the
// Details view.
Item {
    id: view
    property var tab
    property var model: tab.model
    property string mode: "large" // large, medium, small

    readonly property int iconSize: mode === "large" ? 96 : mode === "medium" ? 48 : 16
    readonly property int cellWidth: mode === "large" ? 128 : mode === "medium" ? 100 : 260
    readonly property int cellHeight: mode === "large" ? 150 : mode === "medium" ? 104 : 30
    readonly property int columns: Math.max(1, Math.floor((grid.width - grid.leftMargin) / cellWidth))

    function step(key) {
        const c = model.currentIndex
        const n = model.count
        const rows = Math.max(1, Math.floor(grid.height / cellHeight) - 1)
        if (key === Qt.Key_Left) return Math.max(0, c - 1)
        if (key === Qt.Key_Right) return Math.min(n - 1, c + 1)
        if (key === Qt.Key_Up) return c - columns >= 0 ? c - columns : c
        if (key === Qt.Key_Down) return Math.min(n - 1, c + columns)
        if (key === Qt.Key_PageUp) return Math.max(0, c - columns * rows)
        if (key === Qt.Key_PageDown) return Math.min(n - 1, c + columns * rows)
        return c
    }
    function ensureVisible(row) { grid.positionViewAtIndex(row, GridView.Contain) }

    GridView {
        id: grid
        anchors.fill: parent
        anchors.topMargin: 8
        leftMargin: 12
        rightMargin: 8
        bottomMargin: 24
        model: view.model
        cellWidth: view.cellWidth
        cellHeight: view.cellHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: false // dragging selects
        reuseItems: true
        cacheBuffer: 400
        currentIndex: view.model.currentIndex
        highlightFollowsCurrentItem: false

        MouseArea {
            id: background
            parent: grid
            anchors.fill: parent
            z: -1
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            property point start
            property bool banding: false
            property var base: []
            onPressed: mouse => {
                view.tab.focusView()
                start = Qt.point(mouse.x, mouse.y + grid.contentY)
                banding = false
                base = (mouse.modifiers & Qt.ControlModifier) ? view.model.selectedPaths().map(p => view.model.indexOf(p)) : []
                if (mouse.button === Qt.LeftButton && !(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
                    view.model.clearSelection()
                }
            }
            onPositionChanged: mouse => {
                if (!(mouse.buttons & Qt.LeftButton)) return
                const y = mouse.y + grid.contentY
                if (!banding && Math.abs(y - start.y) + Math.abs(mouse.x - start.x) < 6) return
                banding = true
                band.x = Math.min(start.x, mouse.x)
                band.y = Math.min(start.y, y) - grid.contentY
                band.width = Math.abs(mouse.x - start.x)
                band.height = Math.abs(y - start.y)
                // The cells the rubber band touches.
                const x0 = Math.min(start.x, mouse.x) - grid.leftMargin
                const x1 = Math.max(start.x, mouse.x) - grid.leftMargin
                const r0 = Math.max(0, Math.floor(Math.min(start.y, y) / view.cellHeight))
                const r1 = Math.floor(Math.max(start.y, y) / view.cellHeight)
                const c0 = Math.max(0, Math.floor(x0 / view.cellWidth))
                const c1 = Math.min(view.columns - 1, Math.floor(x1 / view.cellWidth))
                const rows = []
                for (let r = r0; r <= r1; ++r) {
                    for (let c = c0; c <= c1; ++c) {
                        const i = r * view.columns + c
                        if (i < view.model.count) rows.push(i)
                    }
                }
                for (const extra of base) if (rows.indexOf(extra) < 0 && extra >= 0) rows.push(extra)
                view.model.selectRows(rows, false)
            }
            onReleased: { banding = false; band.width = 0 }
            onClicked: mouse => {
                if (mouse.button === Qt.RightButton) {
                    const p = mapToItem(null, mouse.x, mouse.y)
                    view.tab.backgroundMenu(p.x, p.y)
                }
            }
        }

        delegate: Item {
            id: cell
            required property int index
            required property string name
            required property string path
            required property bool isDir
            required property bool selected
            required property bool isCut
            required property bool isLink
            required property string iconName
            required property bool hasThumbnail
            required property var modified
            width: view.cellWidth
            height: view.cellHeight
            readonly property bool small: view.mode === "small"
            // A recycled cell takes its new file's state at once.
            GridView.onPooled: fill.animated = false
            GridView.onReused: fill.animated = true

            Fill {
                id: fill
                objectName: "cellFill"
                anchors { fill: parent; margins: 2 }
                radius: Theme.radius
                color: cellDrop.containsDrag ? Theme.selectionHover
                    : cell.selected ? (cellMouse.containsMouse ? Theme.selectionHover : Theme.selection)
                    : cellMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                border.width: view.model.currentIndex === cell.index && view.tab.viewFocused ? 1 : 0
                border.color: Theme.focusRing
            }

            FileIcon {
                id: icon
                x: cell.small ? 10 : (parent.width - width) / 2
                y: cell.small ? (parent.height - height) / 2 : 8
                size: view.iconSize
                path: cell.path
                iconName: cell.iconName
                thumbnail: cell.hasThumbnail
                modified: cell.modified
                cut: cell.isCut
                link: cell.isLink
            }
            Text {
                visible: view.tab.renamingPath !== cell.path
                x: cell.small ? 34 : 6
                y: cell.small ? (parent.height - height) / 2 : icon.y + icon.height + 6
                width: cell.small ? parent.width - 40 : parent.width - 12
                text: cell.name
                color: Theme.text
                opacity: cell.isCut ? 0.6 : 1
                font.pixelSize: cell.small ? Theme.fontBody : Theme.fontCaption + 1
                horizontalAlignment: cell.small ? Text.AlignLeft : Text.AlignHCenter
                wrapMode: cell.small ? Text.NoWrap : Text.Wrap
                maximumLineCount: cell.small ? 1 : 2
                elide: cell.small ? Text.ElideRight : Text.ElideMiddle
            }
            Loader {
                active: view.tab.renamingPath === cell.path
                x: cell.small ? 30 : 4
                y: cell.small ? 2 : icon.y + icon.height + 2
                width: cell.small ? parent.width - 34 : parent.width - 8
                z: 2
                sourceComponent: RenameField {
                    path: cell.path
                    name: cell.name
                    isDir: cell.isDir
                    centered: !cell.small
                    onDone: newName => view.tab.finishRename(cell.path, newName)
                    onCancelled: view.tab.cancelRename()
                }
            }

            Item {
                id: proxy
                Drag.active: cellMouse.drag.active
                Drag.dragType: Drag.Automatic
                Drag.supportedActions: Qt.CopyAction | Qt.MoveAction | Qt.LinkAction
                Drag.proposedAction: Qt.MoveAction
                Drag.mimeData: ({ "text/uri-list": view.tab.dragUrls(cell.index).join("\r\n") })
                Drag.imageSource: "image://fileicon/" + encodeURIComponent(cell.iconName)
                Drag.imageSourceSize: Qt.size(48, 48)
            }
            MouseArea {
                id: cellMouse
                anchors { fill: parent; margins: 2 }
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                drag.target: proxy
                drag.threshold: 8
                onPressed: mouse => view.tab.pressItem(cell.index, mouse.button, mouse.modifiers)
                onReleased: mouse => { proxy.x = 0; proxy.y = 0; view.tab.releaseItem(cell.index, mouse.button, mouse.modifiers) }
                onDoubleClicked: mouse => { if (mouse.button === Qt.LeftButton) view.tab.activate(cell.index) }
                onClicked: mouse => {
                    if (mouse.button === Qt.RightButton) {
                        const p = mapToItem(null, mouse.x, mouse.y)
                        view.tab.itemMenu(cell.index, p.x, p.y)
                    } else if (mouse.button === Qt.MiddleButton && cell.isDir) {
                        view.tab.openInNewTab(cell.path)
                    }
                }
            }
            DropArea {
                id: cellDrop
                anchors.fill: parent
                enabled: cell.isDir && !cell.selected
                onEntered: drag => { if (!drag.hasUrls) drag.accepted = false }
                onDropped: drop => {
                    Ops.drop(drop.urls.map(u => u.toString()), cell.path, 0)
                    drop.accept(Qt.MoveAction)
                }
            }
        }

        Rectangle {
            id: band
            width: 0
            visible: width > 0
            color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.18)
            border.width: 1
            border.color: Theme.accent
        }

        Text {
            visible: view.model.count === 0 && !view.model.loading
            anchors.horizontalCenter: parent.horizontalCenter
            y: 24
            text: view.model.search !== "" ? qsTr("No items match your search.") : view.model.exists ? qsTr("This folder is empty.") : qsTr("Can't find this folder.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }
    }

    ScrollBar {
        id: scrollBar
        flickable: grid
        anchors { right: grid.right; top: grid.top; bottom: grid.bottom; margins: 2 }
    }
    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            if (event.modifiers & Qt.ControlModifier) {
                view.tab.zoom(event.angleDelta.y > 0 ? 1 : -1)
                return
            }
            scrollBar.scrollBy(event.pixelDelta.y !== 0 ? -event.pixelDelta.y : -event.angleDelta.y / 120 * view.cellHeight)
        }
    }

    DropArea {
        anchors.fill: grid
        z: -2
        onEntered: drag => { if (!drag.hasUrls || !view.tab.writable) drag.accepted = false }
        onDropped: drop => {
            Ops.drop(drop.urls.map(u => u.toString()), view.tab.location, 0)
            drop.accept(Qt.MoveAction)
        }
    }
}
