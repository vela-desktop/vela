// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The overlay is always sized explicitly. A long-running copy must stay
// visible even if an implicit size or Repeater layout would otherwise collapse.
Item {
    id: root
    objectName: "transferProgressPanel"
    width: Math.min(380, Math.max(220, parent ? parent.width - 32 : 380))
    readonly property var jobs: Ops.jobs
    readonly property int jobCount: jobs.length
    readonly property int cardHeight: 116
    readonly property int gap: 8
    height: Math.min(Math.max(0, parent ? parent.height - 96 : 500),
                     jobCount * cardHeight + Math.max(0, jobCount - 1) * gap)
    visible: jobCount > 0

    ListView {
        id: jobList
        objectName: "transferJobsList"
        anchors.fill: parent
        spacing: root.gap
        clip: true
        model: root.jobs
        interactive: contentHeight > height

        delegate: Rectangle {
            id: card
            required property var modelData
            objectName: "transferJob:" + modelData.id
            readonly property real fraction: modelData.total > 0
                ? Math.max(0, Math.min(1, modelData.done / modelData.total)) : 0
            width: ListView.view.width
            height: root.cardHeight
            radius: Theme.radiusOverlay
            color: Theme.dialog
            border.width: 1
            border.color: Theme.dialogStroke

            Text {
                x: 16
                y: 12
                width: parent.width - 64
                height: 36
                text: card.modelData.error !== "" ? card.modelData.error
                    : card.modelData.finished ? card.modelData.doneTitle
                    : card.modelData.title
                color: card.modelData.error !== "" ? Theme.critical : Theme.text
                font.pixelSize: Theme.fontBody
                maximumLineCount: 2
                wrapMode: Text.Wrap
                elide: Text.ElideRight
            }
            ToolButton {
                objectName: "transferCancelButton:" + card.modelData.id
                anchors { right: parent.right; top: parent.top; rightMargin: 8; topMargin: 8 }
                width: 32
                height: 32
                icon.name: "window-close"
                tooltip: card.modelData.finished ? qsTr("Dismiss") : qsTr("Cancel transfer")
                onClicked: card.modelData.finished
                    ? Ops.dismissJob(card.modelData.id) : Ops.cancelJob(card.modelData.id)
            }
            Text {
                x: 16
                y: 53
                width: parent.width - 32
                text: card.modelData.finished
                    ? (card.modelData.error !== "" ? qsTr("Transfer incomplete") : qsTr("Completed"))
                    : card.modelData.total <= 0 ? qsTr("Preparing transfer…")
                    : Math.round(card.fraction * 100) + qsTr("% complete")
                        + (card.modelData.current !== "" ? " · " + card.modelData.current : "")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideMiddle
            }
            Rectangle {
                x: 16
                y: 90
                width: parent.width - 32
                height: 6
                radius: 3
                color: Theme.control
                clip: true
                Rectangle {
                    width: parent.width * card.fraction
                    height: parent.height
                    radius: 3
                    color: card.modelData.error !== "" ? Theme.critical : Theme.accentFill
                    Behavior on width { NumberAnimation { duration: Theme.normal } }
                }
            }
        }
    }
}
