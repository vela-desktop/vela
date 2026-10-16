// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Windows 11 Details view: Name, Date modified, Type, Size (and Location
// in search results); a click on the header sorts, the border between two
// columns can be dragged.
Item {
    id: view
    property var tab
    property var model: tab.model
    readonly property int rowHeight: 32
    readonly property bool searching: model.search !== ""

    // The columns: resizable widths.
    property real nameWidth: Math.max(220, width * 0.38)
    property real dateWidth: 160
    property real typeWidth: 170
    property real sizeWidth: 100
    property real pathWidth: 260
    readonly property var columns: {
        const c = [{ key: 0, title: qsTr("Name"), width: nameWidth }]
        if (searching) c.push({ key: -1, title: qsTr("Location"), width: pathWidth })
        if (tab.isTrash) c.push({ key: -1, title: qsTr("Original location"), width: pathWidth })
        c.push({ key: 1, title: qsTr("Date modified"), width: dateWidth })
        c.push({ key: 2, title: qsTr("Type"), width: typeWidth })
        c.push({ key: 3, title: qsTr("Size"), width: sizeWidth })
        return c
    }
    function columnX(index) {
        let x = 16
        for (let i = 0; i < index; ++i) x += columns[i].width
        return x
    }
    function setColumnWidth(key, w) {
        w = Math.max(60, w)
        if (key === 0) nameWidth = w
        else if (key === 1) dateWidth = w
        else if (key === 2) typeWidth = w
        else if (key === 3) sizeWidth = w
        else pathWidth = w
    }

    // --- for the keyboard and rubber-band selection (TabPage) ---
    function step(key, page) {
        const c = model.currentIndex
        const visible = Math.max(1, Math.floor(list.height / rowHeight) - 1)
        if (key === Qt.Key_Up) return Math.max(0, c - 1)
        if (key === Qt.Key_Down) return Math.min(model.count - 1, c + 1)
        if (key === Qt.Key_PageUp) return Math.max(0, c - visible)
        if (key === Qt.Key_PageDown) return Math.min(model.count - 1, c + visible)
        return c
    }
    function ensureVisible(row) { list.positionViewAtIndex(row, ListView.Contain) }
    function rowRect(row) {
        const p = list.contentItem.mapToItem(view, 0, row * rowHeight)
        return Qt.rect(p.x, p.y, list.width, rowHeight)
    }

    // The header.
    Item {
        id: header
        width: parent.width
        height: 32
        Repeater {
            model: view.columns
            delegate: Item {
                id: column
                required property var modelData
                required property int index
                x: view.columnX(index)
                width: modelData.width
                height: header.height
                Fill {
                    anchors { fill: parent; topMargin: 2; bottomMargin: 2 }
                    radius: Theme.radius
                    color: headerMouse.containsMouse && column.modelData.key >= 0 ? Theme.subtleHover : Theme.subtleRest
                }
                Text {
                    x: 8
                    width: parent.width - 28
                    anchors.verticalCenter: parent.verticalCenter
                    text: column.modelData.title
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
                Text {
                    visible: view.model.sortColumn === column.modelData.key
                    anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                    text: view.model.sortDescending ? "⌄" : "⌃"
                    color: Theme.textSecondary
                    font.pixelSize: 11
                    topPadding: view.model.sortDescending ? -4 : 4
                }
                MouseArea {
                    id: headerMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: column.modelData.key >= 0
                    onClicked: view.model.sortBy(column.modelData.key,
                        view.model.sortColumn === column.modelData.key ? !view.model.sortDescending : false)
                }
                // The right border: drag to widen.
                Rectangle {
                    anchors { right: parent.right; top: parent.top; bottom: parent.bottom; topMargin: 8; bottomMargin: 8 }
                    width: 1
                    color: Theme.divider
                }
                MouseArea {
                    anchors { right: parent.right; rightMargin: -4; top: parent.top; bottom: parent.bottom }
                    width: 8
                    cursorShape: Qt.SplitHCursor
                    property real startX
                    property real startWidth
                    onPressed: mouse => { startX = mapToItem(view, mouse.x, 0).x; startWidth = column.width }
                    onPositionChanged: mouse => view.setColumnWidth(column.modelData.key, startWidth + mapToItem(view, mouse.x, 0).x - startX)
                }
            }
        }
    }

    ListView {
        id: list
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: parent.bottom }
        model: view.model
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true
        cacheBuffer: 320
        currentIndex: view.model.currentIndex
        highlightFollowsCurrentItem: false
        rightMargin: 8
        bottomMargin: 24
        interactive: false // dragging selects; scrolling is by wheel and scroll bar

        // The empty space: click, right button, rubber band, dropped files.
        MouseArea {
            id: background
            parent: list
            anchors.fill: parent
            z: -1
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            property point start
            property bool banding: false
            property var base: []
            onPressed: mouse => {
                view.tab.focusView()
                start = Qt.point(mouse.x, mouse.y + list.contentY)
                banding = false
                base = (mouse.modifiers & Qt.ControlModifier) ? view.model.selectedPaths().map(p => view.model.indexOf(p)) : []
                if (mouse.button === Qt.LeftButton && !(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
                    view.model.clearSelection()
                }
            }
            onPositionChanged: mouse => {
                if (!(mouse.buttons & Qt.LeftButton)) return
                const y = mouse.y + list.contentY
                if (!banding && Math.abs(y - start.y) + Math.abs(mouse.x - start.x) < 6) return
                banding = true
                band.x = Math.min(start.x, mouse.x)
                band.y = Math.min(start.y, y) - list.contentY
                band.width = Math.abs(mouse.x - start.x)
                band.height = Math.abs(y - start.y)
                const first = Math.max(0, Math.floor(Math.min(start.y, y) / view.rowHeight))
                const last = Math.min(view.model.count - 1, Math.floor(Math.max(start.y, y) / view.rowHeight))
                const rows = []
                for (let r = first; r <= last; ++r) rows.push(r)
                view.model.selectRows(view.base(rows, base), false)
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
            id: row
            required property int index
            required property string name
            required property string path
            required property string url
            required property bool isDir
            required property bool selected
            required property bool isCut
            required property bool isLink
            required property string iconName
            required property bool hasThumbnail
            required property var modified
            required property string modifiedText
            required property string type
            required property string sizeText
            required property string location
            width: list.width - 8
            height: view.rowHeight
            // A recycled row takes its new file's state at once.
            ListView.onPooled: fill.animated = false
            ListView.onReused: fill.animated = true

            Fill {
                id: fill
                objectName: "rowFill"
                x: 4
                width: parent.width - 4
                height: parent.height - 2
                y: 1
                radius: Theme.radius
                color: rowDrop.containsDrag ? Theme.selectionHover
                    : row.selected ? (rowMouse.containsMouse ? Theme.selectionHover : Theme.selection)
                    : rowMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                border.width: view.model.currentIndex === row.index && view.tab.viewFocused ? 1 : 0
                border.color: Theme.focusRing
            }

            FileIcon {
                x: view.columnX(0) + 4
                anchors.verticalCenter: parent.verticalCenter
                size: 16
                path: row.path
                iconName: row.iconName
                thumbnail: row.hasThumbnail
                modified: row.modified
                cut: row.isCut
                link: row.isLink
            }
            Text {
                visible: view.tab.renamingPath !== row.path
                x: view.columnX(0) + 30
                width: view.columns[0].width - 34
                anchors.verticalCenter: parent.verticalCenter
                text: row.name
                color: Theme.text
                opacity: row.isCut ? 0.6 : 1
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
            Loader {
                active: view.tab.renamingPath === row.path
                x: view.columnX(0) + 26
                width: view.columns[0].width - 30
                anchors.verticalCenter: parent.verticalCenter
                sourceComponent: RenameField {
                    path: row.path
                    name: row.name
                    isDir: row.isDir
                    onDone: newName => view.tab.finishRename(row.path, newName)
                    onCancelled: view.tab.cancelRename()
                }
            }
            Repeater {
                model: view.columns.slice(1)
                delegate: Text {
                    required property var modelData
                    required property int index
                    x: view.columnX(index + 1) + 8
                    width: modelData.width - 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.key === 1 ? row.modifiedText : modelData.key === 2 ? row.type
                        : modelData.key === 3 ? row.sizeText
                        : view.tab.isTrash ? Ops.originalLocation(row.path).replace(/\/[^\/]*$/, "") : row.location
                    horizontalAlignment: modelData.key === 3 ? Text.AlignRight : Text.AlignLeft
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption + 1
                    elide: modelData.key === -1 ? Text.ElideMiddle : Text.ElideRight
                }
            }

            // Dragging toward other apps (or another folder): the selected
            // files.
            Item {
                id: proxy
                Drag.active: rowMouse.drag.active
                Drag.dragType: Drag.Automatic
                Drag.supportedActions: Qt.CopyAction | Qt.MoveAction | Qt.LinkAction
                Drag.proposedAction: Qt.MoveAction
                Drag.mimeData: ({ "text/uri-list": view.tab.dragUrls(row.index).join("\r\n") })
                Drag.imageSource: "image://fileicon/" + encodeURIComponent(row.iconName)
                Drag.imageSourceSize: Qt.size(32, 32)
            }
            MouseArea {
                id: rowMouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                drag.target: proxy
                drag.threshold: 8
                onPressed: mouse => view.tab.pressItem(row.index, mouse.button, mouse.modifiers)
                onReleased: mouse => { proxy.x = 0; proxy.y = 0; view.tab.releaseItem(row.index, mouse.button, mouse.modifiers) }
                onDoubleClicked: mouse => { if (mouse.button === Qt.LeftButton) view.tab.activate(row.index) }
                onClicked: mouse => {
                    if (mouse.button === Qt.RightButton) {
                        const p = mapToItem(null, mouse.x, mouse.y)
                        view.tab.itemMenu(row.index, p.x, p.y)
                    } else if (mouse.button === Qt.MiddleButton && row.isDir) {
                        view.tab.openInNewTab(row.path)
                    }
                }
            }
            // Folders accept dragged files.
            DropArea {
                id: rowDrop
                anchors.fill: parent
                enabled: row.isDir && !row.selected
                onEntered: drag => { if (!drag.hasUrls) drag.accepted = false }
                onDropped: drop => {
                    Ops.drop(drop.urls.map(u => u.toString()), row.path, 0)
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
            text: view.searching ? qsTr("No items match your search.") : view.model.exists ? qsTr("This folder is empty.") : qsTr("Can't find this folder.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }
    }

    ScrollBar {
        id: scrollBar
        flickable: list
        anchors { right: list.right; top: list.top; bottom: list.bottom; margins: 2 }
    }
    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            if (event.modifiers & Qt.ControlModifier) {
                view.tab.zoom(event.angleDelta.y > 0 ? 1 : -1)
                return
            }
            scrollBar.scrollBy(event.pixelDelta.y !== 0 ? -event.pixelDelta.y : -event.angleDelta.y / 120 * view.rowHeight * 3)
        }
    }

    // With Ctrl: the previous selection plus the rubber band's.
    function base(rows, extra) {
        const all = rows.slice()
        for (const r of extra) if (all.indexOf(r) < 0 && r >= 0) all.push(r)
        return all
    }

    // Files dropped in the folder's space.
    DropArea {
        anchors.fill: list
        z: -2
        onEntered: drag => { if (!drag.hasUrls || !view.tab.writable) drag.accepted = false }
        onDropped: drop => {
            Ops.drop(drop.urls.map(u => u.toString()), view.tab.location, 0)
            drop.accept(Qt.MoveAction)
        }
    }
}
