// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Pdf

// A page of a PDF for the quick preview; only built when QtPdf is there.
Item {
    property string path: ""
    property real detail: 1 // the zoom it is rendered for
    property int page: 0
    readonly property int pages: doc.pageCount
    PdfDocument { id: doc; source: path !== "" ? "file://" + encodeURI(path).replace(/#/g, "%23").replace(/\?/g, "%3F") : "" }
    // Pages without a background of their own are white on paper.
    Rectangle {
        anchors.centerIn: pageImage
        width: pageImage.paintedWidth
        height: pageImage.paintedHeight
        visible: pageImage.status === Image.Ready
        color: "white"
    }
    PdfPageImage {
        id: pageImage
        anchors.fill: parent
        document: doc
        currentFrame: page
        fillMode: Image.PreserveAspectFit
        // Rendered at the page's own shape, fitted to the box: both sides at once would stretch it.
        readonly property size points: doc.status === PdfDocument.Ready ? doc.pagePointSize(page) : Qt.size(1, 1)
        readonly property real fit: Math.min(width / points.width, height / points.height) * parent.detail
        sourceSize: Qt.size(points.width * fit, points.height * fit)
        asynchronous: true
    }
}
