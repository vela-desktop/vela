// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Quick preview (Space): a card over the folder, about 85% of the window, for
// the chosen item. Space or Esc closes it; the arrows keep moving the
// selection in the view and the card follows. Images, text (the first 64 KB),
// PDFs a page at a time (Page Up and Page Down or the buttons), videos and
// sounds playing at once, folders and everything else by icon and properties.
// Images and PDFs zoom with the mouse wheel or a pinch (touchpad, touch
// screen) around the pointer, pan with a drag once zoomed, and a double click
// fits them again.
Item {
    id: peek
    property var tab
    property string path: ""
    property var result: null // answer of Ops.peek() for `path`
    readonly property string kind: result ? result.kind : ""
    property int page: 0 // of a PDF
    readonly property int pages: pdf.item ? pdf.item.pages : 0

    anchors.fill: parent
    visible: false
    z: 100

    // Shown again for the same item: asked again, the old answer stays until the new one.
    function show(p) { if (p === path && p !== "") Ops.peek(p); else path = p; visible = true }
    function hide() { visible = false }
    onPathChanged: { result = null; page = 0; zoomBox.reset(); textFlick.contentY = 0; if (path !== "") Ops.peek(path) }
    onPageChanged: zoomBox.reset()
    function pageBy(n) { page = Math.max(0, Math.min(pages - 1, page + n)) }

    Connections {
        target: Ops
        function onPeekReady(p, r) { if (p === peek.path) peek.result = r }
    }

    function fileUrl(p) { return "file://" + encodeURI(p).replace(/#/g, "%23").replace(/\?/g, "%3F") }

    Rectangle { anchors.fill: parent; color: Theme.smoke
        MouseArea { anchors.fill: parent; onClicked: peek.hide(); onWheel: {} } } // nothing reaches the folder behind

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.round(parent.width * 0.85)
        height: Math.round(parent.height * 0.85)
        radius: Theme.radiusOverlay
        color: Theme.dialog
        border.width: 1
        border.color: Theme.dialogStroke
        clip: true
        MouseArea { anchors.fill: parent; onWheel: {} } // clicks on the card do not close it

        Text {
            id: title
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
            text: peek.result ? peek.result.name : ""
            color: Theme.text
            font.pixelSize: Theme.fontBody + 2
            font.weight: Font.DemiBold
            elide: Text.ElideMiddle
        }
        Item {
            id: body
            anchors { left: parent.left; right: parent.right; top: title.bottom; bottom: meta.top; margins: 12 }

            // Images and PDFs: zoom around the pointer, 1x to 8x.
            Item {
                id: zoomBox
                objectName: "zoomBox"
                anchors.fill: parent
                clip: true
                visible: peek.kind === "image" || peek.kind === "pdf"
                property real zoom: 1
                // Decoded at the next power of two of the zoom, so it stays sharp
                // without decoding again at every wheel step.
                readonly property real detail: Math.pow(2, Math.ceil(Math.log2(zoom) - 0.001))

                function reset() { zoom = 1; content.x = 0; content.y = 0 }
                function panBy(dx, dy) { content.x += dx; content.y += dy; keepInside() }
                function keepInside() {
                    content.x = Math.min(0, Math.max(width - width * zoom, content.x))
                    content.y = Math.min(0, Math.max(height - height * zoom, content.y))
                }
                // Scales by `factor` keeping the point (px, py) of the box still.
                function zoomAt(factor, px, py) {
                    const z = Math.min(8, Math.max(1, zoom * factor))
                    const k = z / zoom
                    content.x = px - (px - content.x) * k
                    content.y = py - (py - content.y) * k
                    zoom = z
                    keepInside()
                }

                Item {
                    id: content
                    objectName: "zoomContent"
                    width: zoomBox.width
                    height: zoomBox.height
                    scale: zoomBox.zoom
                    transformOrigin: Item.TopLeft

                    Image {
                        anchors.fill: parent
                        visible: peek.kind === "image"
                        source: visible ? peek.fileUrl(peek.path) : ""
                        sourceSize: Qt.size(width * zoomBox.detail, height * zoomBox.detail)
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        autoTransform: true
                        smooth: true
                    }
                    Loader {
                        id: pdf
                        anchors.fill: parent
                        active: peek.kind === "pdf"
                        source: active ? "PeekPdf.qml" : ""
                        onLoaded: {
                            item.path = Qt.binding(() => peek.path)
                            item.detail = Qt.binding(() => zoomBox.detail)
                            item.page = Qt.binding(() => peek.page)
                        }
                    }
                }

                PinchHandler {
                    target: null
                    onScaleChanged: delta => zoomBox.zoomAt(delta, centroid.position.x, centroid.position.y)
                    onTranslationChanged: delta => zoomBox.panBy(delta.x, delta.y)
                }
                // The wheel zooms, a drag pans once zoomed, a double click fits again.
                MouseArea {
                    anchors.fill: parent
                    property point last
                    cursorShape: zoomBox.zoom > 1 ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor) : Qt.ArrowCursor
                    onPressed: mouse => last = Qt.point(mouse.x, mouse.y)
                    onPositionChanged: mouse => {
                        if (zoomBox.zoom > 1) zoomBox.panBy(mouse.x - last.x, mouse.y - last.y)
                        last = Qt.point(mouse.x, mouse.y)
                    }
                    onDoubleClicked: zoomBox.reset()
                    onWheel: wheel => zoomBox.zoomAt(Math.pow(1.0015, wheel.angleDelta.y), wheel.x, wheel.y)
                }
            }
            Flickable {
                id: textFlick
                objectName: "textFlick"
                anchors.fill: parent
                visible: peek.kind === "text"
                contentHeight: textView.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Text {
                    id: textView
                    width: parent.width - 14 // room for the scroll bar
                    text: peek.result && peek.result.text !== undefined ? peek.result.text : ""
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.family: "monospace"
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.WrapAnywhere
                }
            }
            // Videos and sounds: only while shown, so closing stops them.
            Loader {
                id: mediaLoader
                objectName: "mediaLoader"
                anchors.fill: parent
                active: peek.visible && peek.kind === "media"
                source: active ? "PeekMedia.qml" : ""
                onLoaded: {
                    item.path = Qt.binding(() => peek.path)
                    item.icon = Qt.binding(() => peek.result ? peek.result.icon : "")
                }
            }
            ScrollBar {
                flickable: textFlick
                visible: textFlick.visible && ratio < 1
                anchors { right: textFlick.right; top: textFlick.top; bottom: textFlick.bottom }
            }
            // Folder, other types, a PDF without QtPdf, or a PDF, video or sound
            // when QtPdf or QtMultimedia is missing at run time: the large icon.
            Column {
                anchors.centerIn: parent
                visible: peek.kind === "folder" || peek.kind === "other"
                    || (peek.kind === "pdf" && pdf.status === Loader.Error)
                    || (peek.kind === "media" && mediaLoader.status === Loader.Error)
                spacing: 12
                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 128
                    height: 128
                    source: peek.result ? "image://fileicon/" + encodeURIComponent(peek.result.icon) : ""
                    sourceSize: Qt.size(width, height)
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: peek.kind === "folder" && !!peek.result
                    text: peek.result ? peek.result.items + qsTr(" items, ") + (peek.result.sizeText || "") : ""
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }
            }
        }
        Row {
            id: pager
            objectName: "pager"
            anchors { right: parent.right; bottom: parent.bottom; margins: 6 }
            visible: peek.kind === "pdf" && peek.pages > 1
            ToolButton { icon.name: "go-previous"; tooltip: qsTr("Previous page (Page Up)"); usable: peek.page > 0; onClicked: peek.pageBy(-1); width: 36 }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: (peek.page + 1) + " / " + peek.pages
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
            }
            ToolButton { icon.name: "go-next"; tooltip: qsTr("Next page (Page Down)"); usable: peek.page < peek.pages - 1; onClicked: peek.pageBy(1); width: 36 }
        }
        Text {
            id: meta
            anchors { left: parent.left; right: pager.visible ? pager.left : parent.right; bottom: parent.bottom; margins: 12 }
            text: {
                const r = peek.result
                if (!r) return ""
                const parts = [r.type]
                if (r.kind !== "folder" && r.sizeText) parts.push(r.sizeText)
                if (r.width) parts.push(r.width + qsTr(" x ") + r.height)
                if (r.modified) parts.push(Qt.formatDateTime(r.modified, "dd/MM/yyyy HH:mm"))
                if (r.truncated) parts.push(qsTr("first 64 KB"))
                return parts.join("  ·  ")
            }
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }
    }
}
