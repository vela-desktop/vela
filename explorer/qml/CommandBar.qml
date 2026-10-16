// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Windows 11 command bar: New; Cut, Copy, Paste, Rename, Share, Delete;
// Sort and View; "…" with the rest. In the Recycle Bin: Empty Recycle Bin and
// Restore.
Item {
    id: bar
    property var tab
    height: 48

    readonly property bool hasSelection: tab.model && tab.model.selectionCount > 0
    readonly property bool writable: tab.isFolder && !tab.isTrash

    function openMenu(entries, item) {
        const p = item.mapToItem(null, 0, item.height)
        tab.menus.open(entries, p.x, p.y + 2)
    }

    Row {
        x: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        ToolButton {
            id: newButton
            visible: !bar.tab.isTrash
            icon.name: "list-add"
            text: qsTr("New")
            menu: true
            usable: bar.writable
            onClicked: bar.openMenu(bar.tab.newEntries(), newButton)
        }
        Rectangle { visible: !bar.tab.isTrash; width: 1; height: 24; anchors.verticalCenter: parent.verticalCenter; color: Theme.divider }
        Item { visible: !bar.tab.isTrash; width: 4; height: 1 }

        ToolButton { visible: !bar.tab.isTrash; icon.name: "edit-cut"; tooltip: qsTr("Cut (Ctrl+X)"); usable: bar.hasSelection && bar.writable; onClicked: bar.tab.cutSelection() }
        ToolButton { visible: !bar.tab.isTrash; icon.name: "edit-copy"; tooltip: qsTr("Copy (Ctrl+C)"); usable: bar.hasSelection; onClicked: bar.tab.copySelection() }
        ToolButton { visible: !bar.tab.isTrash; icon.name: "edit-paste"; tooltip: qsTr("Paste (Ctrl+V)"); usable: Ops.canPaste && bar.writable; onClicked: bar.tab.paste() }
        ToolButton { visible: !bar.tab.isTrash; icon.name: "edit-rename"; tooltip: qsTr("Rename (F2)"); usable: bar.tab.model && bar.tab.model.selectionCount === 1 && bar.writable; onClicked: bar.tab.renameSelection() }
        ToolButton {
            visible: !bar.tab.isTrash
            icon.name: "document-share"
            tooltip: qsTr("Share")
            usable: bar.hasSelection
            onClicked: {
                const m = bar.tab.model
                const files = m.selectedPaths().filter(p => !m.isDirAt(m.indexOf(p)))
                if (files.length > 0) Ops.share(files)
            }
        }
        ToolButton { visible: !bar.tab.isTrash; icon.name: "edit-delete"; tooltip: qsTr("Delete (Del)"); usable: bar.hasSelection && bar.writable; onClicked: bar.tab.deleteSelection(false) }

        // In the Recycle Bin.
        ToolButton { visible: bar.tab.isTrash; icon.name: "trash-empty"; text: qsTr("Empty Recycle Bin"); usable: bar.tab.model && bar.tab.model.count > 0; onClicked: bar.tab.askEmptyTrash() }
        ToolButton { visible: bar.tab.isTrash; icon.name: "edit-undo"; text: qsTr("Restore the selected items"); usable: bar.hasSelection; onClicked: Ops.restoreFromTrash(bar.tab.model.selectedPaths()) }

        Item { width: 4; height: 1 }
        Rectangle { width: 1; height: 24; anchors.verticalCenter: parent.verticalCenter; color: Theme.divider }
        Item { width: 4; height: 1 }

        ToolButton {
            id: sortButton
            icon.name: "view-sort"
            text: qsTr("Sort")
            menu: true
            usable: bar.tab.isFolder
            onClicked: bar.openMenu(bar.tab.sortEntries(), sortButton)
        }
        ToolButton {
            id: viewButton
            icon.name: "view-list-icons"
            text: qsTr("View")
            menu: true
            usable: bar.tab.isFolder
            onClicked: bar.openMenu(bar.tab.viewEntries(), viewButton)
        }
        Item { width: 4; height: 1 }
        Rectangle { width: 1; height: 24; anchors.verticalCenter: parent.verticalCenter; color: Theme.divider }
        Item { width: 4; height: 1 }
        ToolButton {
            id: moreButton
            icon.name: "view-more-horizontal-symbolic"
            tooltip: qsTr("See more")
            usable: bar.tab.isFolder
            onClicked: bar.openMenu(bar.tab.moreEntries(), moreButton)
        }
    }

    // On the right, like Windows 11: the details pane.
    ToolButton {
        anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
        icon.name: "help-about"
        text: qsTr("Details")
        tooltip: qsTr("Details pane (Alt+Shift+P)")
        checked: bar.tab.win.pane === "details"
        usable: bar.tab.isFolder
        onClicked: bar.tab.win.togglePane("details")
    }
}
