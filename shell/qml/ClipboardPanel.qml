// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Clipboard history (Win+V), like Windows 11: what was copied, most recent
// first (pinned first). A click, or Enter, pastes it into the focused app; "…"
// to pin or delete it; Del deletes it. If history is off, the panel offers to
// turn it on.
Window {
    id: root
    visible: false
    // The panel with room for its shadow around it, cut where the window
    // meets the taskbar and the output's edge (docs/visual-coherence.md
    // §6.9): the panel rests 12 units from both.
    readonly property real room: shadow.reach
    // In the clock's corner, wherever the taskbar is (docs/taskbar.md §11.3;
    // main.cpp anchors it there).
    readonly property QtObject bar: Styles.taskbar
    readonly property int edges: bar.trayCorner
    width: bar.pad("left", edges, 12, room) + 360 + bar.pad("right", edges, 12, room)
    height: Math.min(room + panel.height + 12, Screen.height - (bar.vertical ? 0 : bar.thickness))
    color: "transparent"

    readonly property var history: Clip.items
    // The entry the keyboard is on (ClipboardList.qml keeps it on its entry
    // when the history changes).
    property alias current: list.current

    function open() {
        // Reopened while leaving: it turns back from where it is, as it was.
        if (!motion.closing) {
            Menus.placeOnTargetScreen(root)
            current = 0
            list.positionViewAtBeginning()
        }
        visible = true
        motion.open()
        panel.forceActiveFocus()
        Effects.setBlur(root, [motion.visibleArea], Theme.expressive ? Theme.radiusOverlay : 0)
    }
    function close() {
        motion.close()
    }
    // At once, without the exit: the compositor sends Ctrl+V to the app
    // that gets the keyboard back.
    function pasteAt(index) {
        const item = root.history[index]
        if (!item) return
        motion.close(true)
        Clip.paste(item.id)
    }

    Connections {
        target: Shell
        function onClipboardRequested() {
            if (motion.opened) root.close()
            else root.open()
        }
    }
    onActiveChanged: {
        if (!active && motion.opened && !Menus.isOpen) close()
    }

    // In its corner: from panel to output coordinates (menus).
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        const o = bar.origin(edges, root.width, root.height, Screen.width, Screen.height, 0, 0)
        return Qt.point(o.x + p.x, o.y + p.y)
    }

    ShadowRoom {
        panels: [panel]
        onOutside: root.close()
    }

    // It rises 48 units from behind the taskbar, where the window cuts it,
    // like Quick Settings, and leaves the same way.
    PanelMotion {
        id: motion
        x: root.bar.pad("left", root.edges, 12, root.room)
        y: (root.edges & 1) ? 12 : root.height - 12 - height
        width: 360
        height: panel.height
        offset: root.bar.towards(48)
        onClosed: root.visible = false
        onVisibleAreaChanged: Effects.setBlur(root, [visibleArea], Theme.expressive ? Theme.radiusOverlay : 0) // the travel while it moves, the panel at rest

        Shadow {
            id: shadow
            target: panel
        }

        Rectangle {
            id: panel
            width: parent.width
            height: content.height + 32
            radius: Theme.radiusOverlay
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke
            focus: true

            Keys.onPressed: event => {
                const count = root.history.length
                if (event.key === Qt.Key_Escape) {
                    root.close()
                } else if (event.key === Qt.Key_Down && count > 0) {
                    root.current = Math.min(count - 1, root.current + 1)
                    list.positionViewAtIndex(root.current, ListView.Contain)
                } else if (event.key === Qt.Key_Up && count > 0) {
                    root.current = Math.max(0, root.current - 1)
                    list.positionViewAtIndex(root.current, ListView.Contain)
                } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && count > 0) {
                    root.pasteAt(root.current)
                } else if (event.key === Qt.Key_Delete && count > 0) {
                    Clip.remove(root.history[root.current].id)
                } else {
                    return
                }
                event.accepted = true
            }

            Column {
                id: content
                x: 16
                y: 16
                width: parent.width - 32
                spacing: 12

                Item {
                    width: parent.width
                    height: 32
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Clipboard")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody + 2
                        font.weight: Font.DemiBold
                    }
                    Button {
                        visible: Clip.enabled && root.history.some(i => !i.pinned)
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        implicitWidth: implicitContentWidth + 20
                        implicitHeight: 30
                        text: qsTr("Clear all")
                        font.pixelSize: Theme.fontCaption
                        onClicked: { Clip.clear(); root.current = 0 }
                    }
                }

                // Off: like Windows, it's turned on from here.
                Column {
                    visible: !Clip.enabled
                    width: parent.width
                    spacing: 12
                    Text {
                        width: parent.width
                        text: qsTr("Clipboard history")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: qsTr("You can't see your clipboard history. Turn it on to find what you copy here and paste it again.")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.WordWrap
                    }
                    Button {
                        text: qsTr("Turn on")
                        accent: true
                        onClicked: Clip.enabled = true
                    }
                }
                Text {
                    visible: Clip.enabled && root.history.length === 0
                    width: parent.width
                    text: qsTr("There's nothing here. When you copy something, you'll find it here to paste it again.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                }

                ClipboardList {
                    id: list
                    visible: Clip.enabled && root.history.length > 0
                    width: parent.width
                    height: Math.min(contentHeight, 420)
                    entries: root.history
                    onPasteRequested: index => root.pasteAt(index)
                    onMoreRequested: (entry, button) => {
                        const id = entry.id
                        const p = root.screenPoint(button, 0, button.height)
                        Menus.open([
                            { text: entry.pinned ? qsTr("&Unpin") : qsTr("&Pin"), icon: entry.pinned ? "window-unpin" : "window-pin", action: () => Clip.togglePin(id) },
                            { text: qsTr("&Delete"), icon: "edit-delete", shortcut: qsTr("Del"), action: () => Clip.remove(id) }
                        ], p.x, p.y, { screen: root.screen ? root.screen.name : "" })
                    }
                }
            }
        }
    }
}
