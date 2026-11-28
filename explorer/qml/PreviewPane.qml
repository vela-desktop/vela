// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Explorer's right pane, like Windows 11. Two modes:
// - "preview" (Alt+P), the preview pane: the image, the beginning of a text
//   file, the thumbnail of videos and PDFs;
// - "details" (Alt+Shift+P), the details pane: icon, name, type and
//   properties of the chosen item (or of the open folder).
Item {
    id: pane
    property var tab
    property string mode: "preview"
    readonly property var model: tab.model

    // The chosen item: only one; with several items their number is given.
    readonly property int count: model ? model.selectionCount : 0
    readonly property string path: model && count === 1 ? (model.selectionVersion, model.selectedPaths()[0] || "") : ""
    readonly property var info: path !== "" ? Ops.details(path)
        : mode === "details" && count === 0 && tab.isFolder ? Ops.details(tab.location) : null
    readonly property bool file: !!info && !info.isDir
    readonly property string text: mode === "preview" && file ? Ops.previewText(path) : ""
    readonly property bool image: mode === "preview" && file && text === "" && Ops.isImage(path)

    clip: true

    function dateText(d) { return d ? Qt.formatDateTime(d, "dd/MM/yyyy HH:mm") : "" }

    // -------------------------------------------------------- preview --
    Item {
        anchors { fill: parent; margins: 16 }
        visible: pane.mode === "preview"

        Text {
            anchors.centerIn: parent
            width: parent.width
            visible: !pane.file
            text: pane.count > 1 ? pane.count + qsTr(" items selected")
                : pane.info && pane.info.isDir ? qsTr("Select a file to preview.")
                : qsTr("Select a file to preview.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
        // An image, whole.
        Image {
            anchors.fill: parent
            visible: pane.image
            source: pane.image ? "file://" + encodeURI(pane.path).replace(/#/g, "%23").replace(/\?/g, "%3F") : ""
            sourceSize: Qt.size(width, height) // logical: Qt turns it into pixels
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            autoTransform: true
            smooth: true
            mipmap: true
        }
        // A text file: the beginning, as it is.
        Flickable {
            anchors.fill: parent
            visible: pane.text !== ""
            contentHeight: textView.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Text {
                id: textView
                width: parent.width
                text: pane.text
                textFormat: Text.PlainText
                color: Theme.text
                font.family: "monospace"
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WrapAnywhere
            }
        }
        // The others: the thumbnail (video, PDF...) or the large icon.
        Column {
            anchors.centerIn: parent
            width: parent.width
            visible: pane.file && !pane.image && pane.text === ""
            spacing: 12
            Item {
                width: parent.width
                height: Math.min(parent.width, 320)
                Image {
                    id: thumb
                    anchors.fill: parent
                    source: parent.parent.visible
                        ? "image://filethumb/" + encodeURIComponent(pane.path) + "/" + (pane.info.modified ? pane.info.modified.getTime() : 0) : ""
                    sourceSize: Qt.size(512, 512)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false
                }
                Image {
                    anchors.centerIn: parent
                    visible: thumb.status !== Image.Ready
                    width: 96
                    height: 96
                    source: pane.file ? Theme.fileIcons + encodeURIComponent(pane.info.icon) : ""
                    sourceSize: Qt.size(width, height)
                }
            }
            Text {
                width: parent.width
                text: pane.file ? qsTr("No preview available.") : ""
                visible: thumb.status !== Image.Ready
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }

    // --------------------------------------------------------- details --
    Flickable {
        anchors { fill: parent; margins: 16 }
        visible: pane.mode === "details"
        contentHeight: details.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: details
            width: parent.width
            spacing: 12

            Text {
                visible: pane.count > 1
                width: parent.width
                text: pane.count + qsTr(" items selected")
                color: Theme.text
                font.pixelSize: Theme.fontBody + 2
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
            Text {
                visible: pane.count > 1
                text: qsTr("Total size: ") + (pane.model ? Ops.formatSize(pane.model.selectionSize) : "")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }

            // One item (or the open folder).
            Item {
                visible: !!pane.info && pane.count <= 1
                width: parent.width
                height: 128
                Image {
                    id: detailsThumb
                    anchors.fill: parent
                    source: pane.file && pane.info ? "image://filethumb/" + encodeURIComponent(pane.path) + "/"
                        + (pane.info.modified ? pane.info.modified.getTime() : 0) : ""
                    sourceSize: Qt.size(256, 256)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false
                }
                Image {
                    anchors.centerIn: parent
                    visible: detailsThumb.status !== Image.Ready
                    width: 96
                    height: 96
                    source: pane.info ? Theme.fileIcons + encodeURIComponent(pane.info.icon) : ""
                    sourceSize: Qt.size(width, height)
                }
            }
            Text {
                visible: !!pane.info && pane.count <= 1
                width: parent.width
                text: pane.info ? (pane.count === 0 && pane.tab.isFolder ? pane.tab.title : pane.info.name) : ""
                color: Theme.text
                font.pixelSize: Theme.fontBody + 2
                font.weight: Font.DemiBold
                wrapMode: Text.WrapAnywhere
            }
            Text {
                visible: !!pane.info && pane.count <= 1
                width: parent.width
                text: pane.info ? pane.info.type : ""
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
            Rectangle { visible: !!pane.info && pane.count <= 1; width: parent.width; height: 1; color: Theme.divider }
            Text {
                visible: !!pane.info && pane.count <= 1
                text: qsTr("Properties")
                color: Theme.text
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }
            Repeater {
                model: {
                    const i = pane.info
                    if (!i || pane.count > 1) return []
                    const rows = []
                    if (i.sizeText) rows.push([qsTr("Size"), i.sizeText])
                    if (i.width) rows.push([qsTr("Dimensions"), i.width + qsTr(" x ") + i.height])
                    if (i.items !== undefined) rows.push([qsTr("Items"), String(i.items)])
                    rows.push([qsTr("Date modified"), pane.dateText(i.modified)])
                    rows.push([qsTr("Date created"), pane.dateText(i.created)])
                    if (i.target) rows.push([qsTr("Target"), i.target])
                    rows.push([qsTr("Location"), i.location])
                    return rows
                }
                delegate: Column {
                    required property var modelData
                    width: details.width
                    spacing: 2
                    Text {
                        text: modelData[0]
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                    }
                    Text {
                        width: parent.width
                        text: modelData[1]
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }
        }
    }
}
