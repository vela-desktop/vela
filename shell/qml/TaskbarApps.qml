// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Keep app buttons inside their allotted space. Scrolling is explicit so a
// drag still reorders an app, and the wheel still controls its volume.
Item {
    id: root
    required property QtObject style
    property string previousLabel: qsTr("Previous apps")
    property string nextLabel: qsTr("Next apps")
    property real spacing: style.spacing
    readonly property real navigationButtonWidth: 28
    readonly property real minimumWidth: style.buttonWidth + 2 * navigationButtonWidth
    default property alias buttons: row.data
    readonly property bool overflowing: row.width > width
    readonly property real contentX: viewport.contentX
    readonly property real viewportWidth: viewport.width
    readonly property real navigationWidth: overflowing ? Math.min(navigationButtonWidth, width / 3) : 0
    readonly property real maximumX: Math.max(0, row.width - viewport.width)
    implicitWidth: row.width
    implicitHeight: style.buttonHeight
    clip: true

    signal geometryChanged()

    function clampScroll() {
        viewport.contentX = Math.max(0, Math.min(maximumX, viewport.contentX))
    }
    function scroll(direction) {
        const step = style.buttonWidth + spacing
        const page = Math.max(1, Math.floor(viewport.width / step)) * step
        viewport.contentX = Math.max(0, Math.min(maximumX, viewport.contentX + direction * page))
    }
    function reveal(item) {
        if (item.x < viewport.contentX) {
            viewport.contentX = item.x
        } else if (item.x + item.width > viewport.contentX + viewport.width) {
            viewport.contentX = item.x + item.width - viewport.width
        }
        clampScroll()
    }
    function fullyVisible(item) {
        return item.x >= contentX && item.x + item.width <= contentX + viewport.width
    }
    // Hidden apps minimize to the corresponding navigation button, never
    // into the tray. Partially visible buttons use their visible portion.
    function buttonGeometry(item, ancestor) {
        const left = item.x - contentX
        const target = left + item.width <= 0 ? previous : left >= viewport.width ? next : null
        if (target) {
            const p = target.mapToItem(ancestor, 0, 0)
            return Qt.rect(p.x, p.y, target.width, target.height)
        }
        const x = Math.max(0, left)
        const p = viewport.mapToItem(ancestor, x, 0)
        return Qt.rect(p.x, p.y, Math.max(0, Math.min(viewport.width, left + item.width) - x), item.height)
    }

    onXChanged: geometryChanged()
    onYChanged: geometryChanged()
    onWidthChanged: Qt.callLater(clampScroll)
    onMaximumXChanged: Qt.callLater(clampScroll)

    Button {
        id: previous
        objectName: "previousApps"
        visible: root.overflowing
        enabled: root.contentX > 0
        width: root.navigationWidth
        height: parent.height
        subtle: true
        leftPadding: 0
        rightPadding: 0
        icon.name: "go-previous"
        Accessible.name: root.previousLabel
        onClicked: root.scroll(-1)
    }
    Flickable {
        id: viewport
        objectName: "appViewport"
        x: root.navigationWidth
        width: Math.max(0, root.width - 2 * root.navigationWidth)
        height: parent.height
        contentWidth: row.width
        contentHeight: height
        interactive: false
        clip: true
        onContentXChanged: root.geometryChanged()
        onWidthChanged: root.geometryChanged()

        Row {
            id: row
            spacing: root.spacing
            add: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal }
                Eased { property: "scale"; from: 1 - 0.5 * Theme.motion; to: 1; duration: Theme.slow }
            }
            move: Transition { Eased { property: "x"; duration: Theme.normal * Theme.motion } }
        }
    }
    Button {
        id: next
        objectName: "nextApps"
        visible: root.overflowing
        enabled: root.contentX < root.maximumX
        anchors.right: parent.right
        width: root.navigationWidth
        height: parent.height
        subtle: true
        leftPadding: 0
        rightPadding: 0
        icon.name: "go-next"
        Accessible.name: root.nextLabel
        onClicked: root.scroll(1)
    }
}
