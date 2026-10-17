// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Copies and moves in progress, at the bottom right: title, percentage, the
// current file, and the X to cancel; like Windows' progress dialog, in small.
Column {
    id: root
    spacing: 8
    width: 340

    Repeater {
        model: Ops.jobs
        delegate: Rectangle {
            id: card
            required property var modelData
            readonly property real fraction: modelData.total > 0 ? Math.min(1, modelData.done / modelData.total) : 0
            width: root.width
            height: content.height + 24
            radius: Theme.radiusOverlay
            color: Theme.dialog
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 0.45)

            Column {
                id: content
                x: 16
                y: 12
                width: parent.width - 32
                spacing: 6
                Item {
                    width: parent.width
                    height: titleText.height
                    Text {
                        id: titleText
                        width: parent.width - 32
                        text: card.modelData.error !== "" ? card.modelData.error
                            : card.modelData.finished ? card.modelData.doneTitle
                            : card.modelData.title
                        color: card.modelData.error !== "" ? Theme.critical : Theme.text
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    ToolButton {
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        width: 28
                        height: 28
                        icon.name: "window-close"
                        onClicked: card.modelData.finished ? Ops.dismissJob(card.modelData.id) : Ops.cancelJob(card.modelData.id)
                    }
                }
                Text {
                    text: Math.round(card.fraction * 100) + qsTr("% complete") + (card.modelData.current !== "" ? " · " + card.modelData.current : "")
                    visible: !card.modelData.finished
                    width: parent.width
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideMiddle
                }
                Rectangle {
                    width: parent.width
                    height: 4
                    radius: 2
                    color: Theme.control
                    Rectangle {
                        width: parent.width * card.fraction
                        height: parent.height
                        radius: 2
                        color: card.modelData.error !== "" ? Theme.critical : Theme.accentFill
                        Behavior on width { NumberAnimation { duration: Theme.normal } }
                    }
                }
            }
        }
    }
}
