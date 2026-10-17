// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Windows 11 address bar: Back, Forward, Up, Refresh; the path in pieces
// (each piece goes there, the arrow between pieces opens the subfolders); a
// click on the empty part (or Ctrl+L) makes it editable; the search box on the
// right.
Item {
    id: bar
    property var tab
    height: 48

    function focusAddress() {
        editor.text = tab.isFolder ? tab.location : ""
        editing = true
        editor.forceActiveFocus()
        editor.selectAll()
    }
    function focusSearch() {
        search.forceActiveFocus()
        search.selectAll()
    }
    property bool editing: false

    // The path pieces: [{label, location, icon}].
    readonly property var crumbs: {
        const loc = tab.location
        if (loc === "home:") return [{ label: qsTr("Home"), location: "home:", icon: "go-home" }]
        if (loc === "thispc:") return [{ label: qsTr("This PC"), location: "thispc:", icon: "computer" }]
        if (loc === Places.trash) return [{ label: qsTr("Recycle Bin"), location: Places.trash, icon: "user-trash" }]
        const out = []
        let rest
        if (loc === Places.home || loc.startsWith(Places.home + "/")) {
            out.push({ label: Places.userName, location: Places.home, icon: "user-home" })
            rest = loc.slice(Places.home.length)
        } else {
            out.push({ label: qsTr("This PC"), location: "thispc:", icon: "computer" })
            out.push({ label: qsTr("Local Disk"), location: "/" })
            rest = loc
        }
        let path = out[out.length - 1].location === "/" ? "" : out[out.length - 1].location
        for (const part of rest.split("/").filter(p => p !== "")) {
            path += "/" + part
            out.push({ label: part, location: path })
        }
        return out
    }

    Row {
        id: buttons
        x: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2
        ToolButton { icon.name: "go-previous"; tooltip: qsTr("Back (Alt+Left Arrow)"); usable: bar.tab.canGoBack; onClicked: bar.tab.back(); width: 36 }
        ToolButton { icon.name: "go-next"; tooltip: qsTr("Forward (Alt+Right Arrow)"); usable: bar.tab.canGoForward; onClicked: bar.tab.forward(); width: 36 }
        ToolButton { icon.name: "go-up"; tooltip: qsTr("Up (Alt+Up Arrow)"); usable: bar.tab.canGoUp; onClicked: bar.tab.up(); width: 36 }
        ToolButton { icon.name: "view-refresh"; tooltip: qsTr("Refresh (F5)"); onClicked: bar.tab.refresh(); width: 36 }
    }

    Fill {
        id: address
        anchors { left: buttons.right; leftMargin: 8; right: searchBox.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
        height: 32
        radius: Theme.radius
        color: bar.editing ? Theme.field : addressMouse.containsMouse ? Theme.controlHover : Theme.control
        border.width: 1
        border.color: bar.editing ? Theme.accent : Theme.controlStroke
        clip: true

        MouseArea {
            id: addressMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: bar.focusAddress()
        }

        // The path in pieces, right-aligned when too long (like Windows).
        Flickable {
            visible: !bar.editing
            anchors { fill: parent; leftMargin: 4; rightMargin: 4 }
            contentWidth: crumbRow.width
            contentX: Math.max(0, crumbRow.width - width)
            interactive: false
            Row {
                id: crumbRow
                height: parent.height
                Repeater {
                    model: bar.crumbs
                    delegate: Row {
                        id: crumb
                        required property var modelData
                        required property int index
                        height: parent.height
                        Fill {
                            height: 26
                            anchors.verticalCenter: parent.verticalCenter
                            width: crumbContent.width + 12
                            radius: Theme.radius
                            color: crumbMouse.containsMouse || crumbDrop.containsDrag ? Theme.subtleHover : Theme.subtleRest
                            Row {
                                id: crumbContent
                                anchors.centerIn: parent
                                spacing: 6
                                Image {
                                    visible: !!crumb.modelData.icon
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 16
                                    height: 16
                                    source: crumb.modelData.icon ? "image://fileicon/" + crumb.modelData.icon : ""
                                    sourceSize: Qt.size(width, height)
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: crumb.modelData.label
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBody
                                }
                            }
                            MouseArea {
                                id: crumbMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                                onClicked: mouse => mouse.button === Qt.MiddleButton ? bar.tab.openInNewTab(crumb.modelData.location)
                                                                                     : bar.tab.navigate(crumb.modelData.location)
                            }
                            DropArea {
                                id: crumbDrop
                                anchors.fill: parent
                                enabled: crumb.modelData.location.startsWith("/")
                                onEntered: drag => { if (!drag.hasUrls) drag.accepted = false }
                                onDropped: drop => {
                                    Ops.drop(drop.urls.map(u => u.toString()), crumb.modelData.location, 0)
                                    drop.accept(Qt.MoveAction)
                                }
                            }
                        }
                        // The arrow: this piece's subfolders.
                        Fill {
                            visible: crumb.modelData.location.startsWith("/") && crumb.modelData.location !== Places.trash
                            height: 26
                            width: 18
                            anchors.verticalCenter: parent.verticalCenter
                            radius: Theme.radius
                            color: chevronMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                            Chevron {
                                anchors.centerIn: parent
                                color: Theme.textSecondary
                            }
                            MouseArea {
                                id: chevronMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    const base = crumb.modelData.location
                                    const names = Ops.subfolders(base, bar.tab.showHidden)
                                    const entries = names.slice(0, 60).map(n => ({
                                        text: n.replace(/&/g, "&&"), icon: "folder",
                                        action: () => bar.tab.navigate((base === "/" ? "" : base) + "/" + n)
                                    }))
                                    if (entries.length === 0) entries.push({ text: qsTr("No folders"), enabled: false })
                                    const p = mapToItem(null, 0, height)
                                    bar.tab.menus.open(entries, p.x, p.y + 2)
                                }
                            }
                        }
                    }
                }
            }
        }

        TextInput {
            id: editor
            visible: bar.editing
            anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.text
            selectionColor: Theme.accent
            selectedTextColor: "white"
            font.pixelSize: Theme.fontBody
            selectByMouse: true
            clip: true
            Keys.onEscapePressed: { bar.editing = false; bar.tab.focusView() }
            Keys.onReturnPressed: {
                const path = Ops.resolve(text, bar.tab.location)
                if (path !== "") {
                    bar.editing = false
                    bar.tab.navigate(path)
                    bar.tab.focusView()
                } else {
                    bar.tab.showError(qsTr("Can't find \"") + text + qsTr("\". Check the spelling and try again."))
                }
            }
            onActiveFocusChanged: if (!activeFocus) bar.editing = false
        }
    }

    // Search: in this folder and its subfolders.
    Fill {
        id: searchBox
        anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
        width: Math.min(280, bar.width * 0.25)
        height: 32
        radius: Theme.radius
        color: search.activeFocus ? Theme.field : searchMouse.containsMouse ? Theme.controlHover : Theme.control
        border.width: 1
        border.color: search.activeFocus ? Theme.accent : Theme.controlStroke
        opacity: bar.tab.isFolder ? 1 : 0.5
        MouseArea {
            id: searchMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: search.forceActiveFocus()
        }
        Text {
            visible: search.text === ""
            anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 32 }
            text: qsTr("Search ") + bar.tab.title
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
        TextInput {
            id: search
            anchors { fill: parent; leftMargin: 10; rightMargin: 32 }
            verticalAlignment: TextInput.AlignVCenter
            enabled: bar.tab.isFolder
            color: Theme.text
            selectionColor: Theme.accent
            selectedTextColor: "white"
            font.pixelSize: Theme.fontBody
            selectByMouse: true
            clip: true
            onTextChanged: searchDelay.restart()
            Keys.onEscapePressed: { text = ""; bar.tab.focusView() }
            Keys.onReturnPressed: { searchDelay.stop(); bar.tab.search(text) }
            Timer { id: searchDelay; interval: 350; onTriggered: bar.tab.search(search.text) }
            Connections {
                target: bar.tab
                function onLocationChanged() { search.text = ""; searchDelay.stop() }
            }
        }
        Icon {
            anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
            name: "edit-find"
            opacity: 0.8
        }
    }
}
