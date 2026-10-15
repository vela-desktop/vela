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
    width: 384
    height: panel.height + 24
    color: "transparent"

    property int current: 0

    function open() {
        Menus.placeOnTargetScreen(root)
        current = 0
        visible = true
        panel.opacity = 0
        slide.y = 12 * Theme.motion
        appear.restart()
        panel.forceActiveFocus()
        updateBlur()
    }
    function close() {
        visible = false
    }
    function pasteAt(index) {
        const item = Clip.items[index]
        if (!item) return
        close()
        Clip.paste(item.id)
    }

    Connections {
        target: Shell
        function onClipboardRequested() {
            if (root.visible) root.close()
            else root.open()
        }
    }
    onActiveChanged: {
        if (!active && visible && !Menus.isOpen) close()
    }

    function updateBlur() {
        Effects.setBlur(root, [Qt.rect(panel.x, panel.y, panel.width, panel.height)])
    }

    // At the bottom right, above the taskbar: from panel to output coordinates
    // (menus).
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        return Qt.point(Screen.width - root.width + p.x, Screen.height - Theme.taskbarHeight - root.height + p.y)
    }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: panel; property: "opacity"; to: 1; duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        NumberAnimation { target: slide; property: "y"; to: 0; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }

    Rectangle {
        id: panel
        x: 12
        y: 12
        width: root.width - 24
        height: content.height + 32
        radius: Theme.radiusOverlay
        color: Theme.surface
        border.width: 1
        border.color: Theme.stroke
        transform: Translate { id: slide }
        focus: true
        onHeightChanged: root.updateBlur()

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
                Rectangle {
                    visible: Clip.enabled && Clip.items.some(i => !i.pinned)
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    width: clearText.implicitWidth + 20
                    height: 30
                    radius: Theme.radius
                    color: clearMouse.containsMouse ? Theme.subtleHover : Theme.control
                    border.width: 1
                    border.color: Theme.stroke
                    Text {
                        id: clearText
                        anchors.centerIn: parent
                        text: qsTr("Clear all")
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
                    }
                    MouseArea {
                        id: clearMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: { Clip.clear(); root.current = 0 }
                    }
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
                        delegate: Rectangle {
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
                            Rectangle {
                                id: more
                                anchors { right: parent.right; top: parent.top; margins: 8 }
                                width: 28
                                height: 28
                                radius: Theme.radius
                                color: moreMouse.containsMouse ? Theme.subtleHover : "transparent"
                                Text {
                                    anchors.centerIn: parent
                                    text: "…"
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBody
                                }
                                MouseArea {
                                    id: moreMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
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
