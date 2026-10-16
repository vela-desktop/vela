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
    width: room + 360 + 12
    height: Math.min(room + panel.height + 12, Screen.height - Theme.taskbarHeight)
    color: "transparent"

    property int current: 0

    function open() {
        // Reopened while leaving: it turns back from where it is, as it was.
        if (!motion.closing) {
            Menus.placeOnTargetScreen(root)
            current = 0
        }
        visible = true
        motion.open()
        panel.forceActiveFocus()
        Effects.setBlur(root, [motion.visibleArea])
    }
    function close() {
        motion.close()
    }
    // At once, without the exit: the compositor sends Ctrl+V to the app
    // that gets the keyboard back.
    function pasteAt(index) {
        const item = Clip.items[index]
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

    // At the bottom right, above the taskbar: from panel to output coordinates
    // (menus).
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        return Qt.point(Screen.width - root.width + p.x, Screen.height - Theme.taskbarHeight - root.height + p.y)
    }

    ShadowRoom {
        panels: [panel]
        onOutside: root.close()
    }

    // It rises 48 units from behind the taskbar, where the window cuts it,
    // like Quick Settings, and leaves the same way.
    PanelMotion {
        id: motion
        x: root.room
        y: root.height - 12 - height
        width: 360
        height: panel.height
        offset: Qt.point(0, 48)
        onClosed: root.visible = false
        onVisibleAreaChanged: Effects.setBlur(root, [visibleArea]) // the travel while it moves, the panel at rest

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
                const count = Clip.items.length
                if (event.key === Qt.Key_Escape) {
                    root.close()
                } else if (event.key === Qt.Key_Down && count > 0) {
                    root.current = Math.min(count - 1, root.current + 1)
                    list.positionAt(root.current)
                } else if (event.key === Qt.Key_Up && count > 0) {
                    root.current = Math.max(0, root.current - 1)
                    list.positionAt(root.current)
                } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && count > 0) {
                    root.pasteAt(root.current)
                } else if (event.key === Qt.Key_Delete && count > 0) {
                    Clip.remove(Clip.items[root.current].id)
                    root.current = Math.max(0, Math.min(root.current, Clip.items.length - 1))
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
                        visible: Clip.enabled && Clip.items.some(i => !i.pinned)
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
                    visible: Clip.enabled && Clip.items.length === 0
                    width: parent.width
                    text: qsTr("There's nothing here. When you copy something, you'll find it here to paste it again.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                }

                Flickable {
                    id: list
                    visible: Clip.enabled && Clip.items.length > 0
                    width: parent.width
                    height: Math.min(contentHeight, 420)
                    contentHeight: cards.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    function positionAt(index) {
                        const card = cards.children[index]
                        if (!card) return
                        if (card.y < contentY) contentY = card.y
                        else if (card.y + card.height > contentY + height) contentY = card.y + card.height - height
                    }

                    Column {
                        id: cards
                        width: parent.width
                        spacing: 8
                        Repeater {
                            model: Clip.items
                            delegate: Fill {
                                id: card
                                required property var modelData
                                required property int index
                                readonly property bool selected: index === root.current
                                width: cards.width
                                height: Math.max(56, (modelData.kind === "image" ? thumb.height : body.height) + 24)
                                radius: Theme.radius
                                color: cardMouse.containsMouse ? Theme.subtleHover : Theme.control
                                border.width: selected ? 2 : 1
                                border.color: selected ? Theme.accent : Theme.stroke

                                Text {
                                    id: body
                                    visible: card.modelData.kind === "text"
                                    x: 12
                                    y: 12
                                    width: parent.width - 56
                                    text: card.modelData.text
                                    textFormat: Text.PlainText
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBody
                                    wrapMode: Text.WrapAnywhere
                                    maximumLineCount: 4
                                    elide: Text.ElideRight
                                }
                                Image {
                                    id: thumb
                                    visible: card.modelData.kind === "image"
                                    x: 12
                                    y: 12
                                    width: parent.width - 56
                                    height: 96
                                    fillMode: Image.PreserveAspectFit
                                    horizontalAlignment: Image.AlignLeft
                                    source: visible ? "image://clipboard/" + card.modelData.id : ""
                                    sourceSize: Qt.size(width, height) // logical: Qt turns it into pixels
                                    asynchronous: true
                                }
                                MouseArea {
                                    id: cardMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: root.pasteAt(card.index)
                                }
                                // Pinned: the pin; "…" to pin or delete.
                                Text {
                                    visible: card.modelData.pinned
                                    anchors { right: more.left; rightMargin: 2; verticalCenter: more.verticalCenter }
                                    text: "\u{1F4CC}"
                                    font.pixelSize: 11
                                    opacity: 0.7
                                }
                                Button {
                                    id: more
                                    anchors { right: parent.right; top: parent.top; margins: 8 }
                                    width: 28
                                    height: 28
                                    padding: 0
                                    subtle: true
                                    text: "…"
                                    Accessible.name: qsTr("See more")
                                    onClicked: {
                                        const id = card.modelData.id
                                        const p = root.screenPoint(more, 0, more.height)
                                        Menus.open([
                                            { text: card.modelData.pinned ? qsTr("&Unpin") : qsTr("&Pin"), icon: card.modelData.pinned ? "window-unpin" : "window-pin", action: () => Clip.togglePin(id) },
                                            { text: qsTr("&Delete"), icon: "edit-delete", shortcut: qsTr("Del"), action: () => Clip.remove(id) }
                                        ], p.x, p.y, { screen: root.screen ? root.screen.name : "" })
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
