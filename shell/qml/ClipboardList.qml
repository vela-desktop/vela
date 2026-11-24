// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Clipboard history's entries (ClipboardPanel.qml), pinned first, each most
// recent first: each one's text or image, its pin and "…". A click asks to
// paste it, "…" asks for its menu; `current` is the one the keyboard is on.
ListView {
    id: list
    // Clip.items: each entry is { id, kind, text, pinned }.
    property var entries: []
    // The entry that Up and Down move, Enter pastes and Del deletes.
    property int current: 0
    signal pasteRequested(int index)
    signal moreRequested(var entry, Item button)

    clip: true
    boundsBehavior: Flickable.StopAtBounds
    spacing: 8
    currentIndex: -1

    // Each change to the history is a new list, and a new model takes the
    // view back to the top. Instead, the view stays where it was, and the
    // selection stays on its entry, or on the one now in its place if it
    // was deleted. A selection that was in sight stays in sight.
    // `shown` is what the view shows: the entries as they were last.
    property var shown: []
    onEntriesChanged: {
        const kept = shown[current]
        const item = itemAtIndex(current)
        const seen = item !== null && item.y + item.height > contentY && item.y < contentY + height
        const y = contentY
        shown = entries
        model = entries
        const index = kept ? entries.findIndex(e => e.id === kept.id) : -1
        current = index >= 0 ? index : Math.max(0, Math.min(current, entries.length - 1))
        contentY = Math.max(originY, Math.min(y, originY + contentHeight - height))
        if (seen && entries.length > 0) positionViewAtIndex(current, ListView.Contain)
    }

    delegate: Fill {
        id: card
        required property var modelData
        required property int index
        readonly property bool selected: index === list.current
        width: list.width
        height: Math.max(56, (modelData.kind === "image" ? thumb.height : body.height) + 24)
        radius: Theme.nested(Theme.radiusOverlay, 16, Theme.radius) // 16 inside the panel
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
            onClicked: list.pasteRequested(card.index)
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
            onClicked: list.moreRequested(card.modelData, more)
        }
    }
}
