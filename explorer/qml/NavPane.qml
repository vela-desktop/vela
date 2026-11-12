// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The navigation pane on the left, like Windows 11: Home, the Quick access
// folders (with the pin), This PC with the drives (also unmounted ones,
// mounted when opened), the Recycle Bin. Files can be dropped on an entry to
// move or copy them there.
Flickable {
    id: pane
    property string current // the open location, to highlight it
    signal navigate(string location)
    signal openInNewTab(string location)
    signal openVolume(string volume) // an unmounted drive: mounted and opened
    signal contextMenu(var entries, real x, real y)

    contentHeight: column.height + 16
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    // Expressive: M3's navigation drawer item at Vela's 32 rows: the place
    // in use a full pill of secondary container with its text in Medium, no
    // bar; the others state layers.
    component Entry: Fill {
        id: entry
        readonly property bool expressive: Theme.expressive
        property string icon
        property string label
        property string location
        property bool pinned: false
        property string volume: "" // unmounted drive (udisks)
        property string device: "" // mounted removable drive: it can be ejected
        property bool droppable: location.startsWith("/")
        property int indent: 0
        readonly property bool selected: pane.current === location
        objectName: "place:" + location
        width: column.width
        height: 32
        radius: expressive ? height / 2 : Theme.radius
        color: drop.containsDrag ? Theme.selectionHover : selected ? Theme.selection : mouse.containsMouse ? Theme.hoverLayer : Theme.restLayer

        Rectangle {
            objectName: "selectionBar"
            visible: entry.selected && !entry.expressive
            anchors.verticalCenter: parent.verticalCenter
            width: 3
            height: 16
            radius: 1.5
            color: Theme.accentFill
        }
        Image {
            x: 12 + entry.indent
            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            source: "image://fileicon/" + encodeURIComponent(entry.icon)
            sourceSize: Qt.size(width, height)
        }
        Text {
            objectName: "label"
            x: 38 + entry.indent
            width: parent.width - x - (entry.pinned ? 28 : 8)
            anchors.verticalCenter: parent.verticalCenter
            text: entry.label
            color: entry.expressive && entry.selected ? Theme.secondaryContainerText : Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: entry.expressive && entry.selected ? Theme.weightLabel : Font.Normal
            elide: Text.ElideRight
        }
        Text {
            visible: entry.pinned
            anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
            text: "\u{1F4CC}"
            font.pixelSize: 10
            opacity: 0.55
        }
        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            onClicked: mouse => {
                if (mouse.button === Qt.MiddleButton) {
                    pane.openInNewTab(entry.location)
                } else if (mouse.button === Qt.RightButton) {
                    const p = entry.mapToItem(null, mouse.x, mouse.y)
                    pane.contextMenu(pane.entryMenu(entry), p.x, p.y)
                } else if (entry.volume !== "") {
                    pane.openVolume(entry.volume)
                } else {
                    pane.navigate(entry.location)
                }
            }
        }
        DropArea {
            id: drop
            anchors.fill: parent
            enabled: entry.droppable
            onEntered: drag => { if (!drag.hasUrls) drag.accepted = false }
            onDropped: drop => {
                Ops.drop(drop.urls.map(u => u.toString()), entry.location, 0)
                drop.accept(Qt.MoveAction)
            }
        }
    }

    // An entry's menu: Open, Open in new tab, the pin, Properties.
    function entryMenu(entry) {
        const loc = entry.location
        if (entry.volume !== "") {
            return [{ text: qsTr("&Open"), icon: "document-open", action: () => pane.openVolume(entry.volume) }]
        }
        const entries = [
            { text: qsTr("&Open"), icon: "document-open", action: () => pane.navigate(loc) },
            { text: qsTr("Open in new &tab"), icon: "tab-new", action: () => pane.openInNewTab(loc) },
            { text: qsTr("Open in new &window"), icon: "window-new", action: () => Ops.newWindow(loc) }
        ]
        if (loc.startsWith("/") && loc !== Places.trash) {
            entries.push({ separator: true })
            entries.push(Places.isPinned(loc)
                ? { text: qsTr("&Unpin from Quick access"), icon: "window-unpin", action: () => Places.unpin(loc) }
                : { text: qsTr("Pin to &Quick access"), icon: "window-pin", action: () => Places.pin(loc) })
            entries.push({ text: qsTr("Open in &Terminal"), icon: "utilities-terminal", action: () => System.openTerminal(loc) })
            if (entry.device !== "") {
                entries.push({ text: qsTr("E&ject"), icon: "media-eject", action: () => Places.eject(entry.device) })
            }
            entries.push({ separator: true })
            entries.push({ text: qsTr("P&roperties"), icon: "document-properties", action: () => Ops.showProperties([loc]) })
        }
        if (loc === Places.trash) {
            entries.push({ separator: true })
            entries.push({ text: qsTr("&Empty Recycle Bin"), icon: "trash-empty", action: () => Ops.emptyTrash() })
        }
        return entries
    }

    Column {
        id: column
        x: 8
        y: 8
        width: pane.width - 16
        spacing: 2

        Entry { icon: "go-home"; label: qsTr("Home"); location: "home:" }
        Item { width: 1; height: 8 }
        Repeater {
            model: Places.quickAccess
            delegate: Entry {
                required property var modelData
                icon: modelData.icon
                label: modelData.name
                location: modelData.path
                pinned: true
            }
        }
        Rectangle { width: parent.width; height: 1; color: Theme.divider }
        Entry { icon: "computer"; label: qsTr("This PC"); location: "thispc:" }
        Repeater {
            model: Places.drives
            delegate: Entry {
                required property var modelData
                icon: modelData.icon
                label: modelData.name
                location: modelData.path
                volume: modelData.volume || ""
                device: modelData.mounted && modelData.removable ? modelData.device : ""
                opacity: modelData.mounted ? 1 : 0.7
                indent: 16
            }
        }
        Entry { icon: "user-home"; label: Places.userName; location: Places.home; indent: 16 }
        Rectangle { width: parent.width; height: 1; color: Theme.divider }
        Entry { icon: "user-trash"; label: qsTr("Recycle Bin"); location: Places.trash }
    }
}
