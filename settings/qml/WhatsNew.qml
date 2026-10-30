// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls
import Vela.Updates

// System > Updates' "What's new" (docs/updates.md §9.1), for the update
// found: Beta's release texts, or Experimental's commits (the first 10,
// then "and N more"). Plain text: they come from the repository.
CardGroup {
    id: group
    objectName: "whatsNew"
    readonly property var shown: Updates.changes.slice(0, 10)
    readonly property int more: Math.max(0, Updates.changesTotal - shown.length)

    title: qsTr("What's new")
    visible: Updates.available && Updates.status === "update-available"
        && (Updates.notes.length > 0 || Updates.changes.length > 0)

    Card {
        objectName: "notes"
        visible: Updates.notes.length > 0
        contentItem: Column {
            width: parent.width
            spacing: 16
            Repeater {
                model: Updates.notes
                delegate: Column {
                    required property var modelData
                    width: parent.width
                    spacing: 4
                    Text {
                        width: parent.width
                        text: qsTr("Vela %1").arg(modelData.version)
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: modelData.text
                        textFormat: Text.PlainText
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    Card {
        objectName: "changes"
        visible: Updates.notes.length === 0 && Updates.changes.length > 0
        contentItem: Column {
            width: parent.width
            spacing: 8
            Repeater {
                model: group.shown
                delegate: Item {
                    required property var modelData
                    width: parent.width
                    height: Math.max(summary.height, hash.height)
                    Text {
                        id: summary
                        width: parent.width - hash.width - 16
                        text: modelData.summary
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }
                    Text {
                        id: hash
                        anchors.right: parent.right
                        text: modelData.commit.slice(0, 7)
                        textFormat: Text.PlainText
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                        font.family: "monospace"
                    }
                }
            }
            Text {
                objectName: "more"
                visible: group.more > 0
                width: parent.width
                text: qsTr("and %n more", "", group.more)
                textFormat: Text.PlainText
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }
        }
    }
}
