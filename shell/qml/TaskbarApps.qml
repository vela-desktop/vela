// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Keep app buttons inside their allotted space. Scrolling is explicit so a
// drag still reorders an app, and the wheel still controls its volume. A row,
// or a column on a vertical taskbar (docs/taskbar.md §11.2), paged up and
// down.
Item {
    id: root
    required property QtObject style
    property bool vertical: false
    property string previousLabel: qsTr("Previous apps")
    property string nextLabel: qsTr("Next apps")
    property real spacing: style.spacing
    readonly property real navigationButtonWidth: 28
    readonly property real step: (vertical ? style.buttonHeight : style.buttonWidth) + spacing
    readonly property real minimumWidth: style.buttonWidth + 2 * navigationButtonWidth
    readonly property real minimumHeight: style.buttonHeight + 2 * navigationButtonWidth
    default property alias buttons: row.data
    // Along the strip: its length, the content's, where it is scrolled to.
    readonly property real length: vertical ? height : width
    readonly property real contentLength: vertical ? row.height : row.width
    readonly property bool overflowing: contentLength > length
    readonly property real contentX: vertical ? viewport.contentY : viewport.contentX
    readonly property real viewportWidth: vertical ? viewport.height : viewport.width
    readonly property real navigationWidth: overflowing ? Math.min(navigationButtonWidth, length / 3) : 0
    readonly property real maximumX: Math.max(0, contentLength - viewportWidth)
    implicitWidth: vertical ? style.buttonWidth : row.width
    implicitHeight: vertical ? row.height : style.buttonHeight
    clip: true

    signal geometryChanged()

    function setScroll(value) {
        const clamped = Math.max(0, Math.min(maximumX, value))
        if (vertical) viewport.contentY = clamped
        else viewport.contentX = clamped
    }
    function clampScroll() {
        setScroll(contentX)
    }
    function scroll(direction) {
        const page = Math.max(1, Math.floor(viewportWidth / step)) * step
        setScroll(contentX + direction * page)
    }
    function startOf(item) { return vertical ? item.y : item.x }
    function lengthOf(item) { return vertical ? item.height : item.width }
    function reveal(item) {
        if (startOf(item) < contentX) {
            setScroll(startOf(item))
        } else if (startOf(item) + lengthOf(item) > contentX + viewportWidth) {
            setScroll(startOf(item) + lengthOf(item) - viewportWidth)
        }
    }
    function fullyVisible(item) {
        return startOf(item) >= contentX && startOf(item) + lengthOf(item) <= contentX + viewportWidth
    }
    // Hidden apps minimize to the corresponding navigation button, never
    // into the tray. Partially visible buttons use their visible portion.
    function buttonGeometry(item, ancestor) {
        const start = startOf(item) - contentX
        const target = start + lengthOf(item) <= 0 ? previous : start >= viewportWidth ? next : null
        if (target) {
            const p = target.mapToItem(ancestor, 0, 0)
            return Qt.rect(p.x, p.y, target.width, target.height)
        }
        const from = Math.max(0, start)
        const shown = Math.max(0, Math.min(viewportWidth, start + lengthOf(item)) - from)
        const p = vertical ? viewport.mapToItem(ancestor, 0, from) : viewport.mapToItem(ancestor, from, 0)
        return vertical ? Qt.rect(p.x, p.y, item.width, shown) : Qt.rect(p.x, p.y, shown, item.height)
    }

    onXChanged: geometryChanged()
    onYChanged: geometryChanged()
    onWidthChanged: Qt.callLater(clampScroll)
    onHeightChanged: Qt.callLater(clampScroll)
    onMaximumXChanged: Qt.callLater(clampScroll)

    Button {
        id: previous
        objectName: "previousApps"
        visible: root.overflowing
        enabled: root.contentX > 0
        width: root.vertical ? parent.width : root.navigationWidth
        height: root.vertical ? root.navigationWidth : parent.height
        subtle: true
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        icon.name: root.vertical ? "go-up" : "go-previous"
        Accessible.name: root.previousLabel
        onClicked: root.scroll(-1)
    }
    Flickable {
        id: viewport
        objectName: "appViewport"
        x: root.vertical ? 0 : root.navigationWidth
        y: root.vertical ? root.navigationWidth : 0
        width: root.vertical ? parent.width : Math.max(0, root.width - 2 * root.navigationWidth)
        height: root.vertical ? Math.max(0, root.height - 2 * root.navigationWidth) : parent.height
        contentWidth: root.vertical ? width : row.width
        contentHeight: root.vertical ? row.height : height
        interactive: false
        clip: true
        onContentXChanged: root.geometryChanged()
        onContentYChanged: root.geometryChanged()
        onWidthChanged: root.geometryChanged()
        onHeightChanged: root.geometryChanged()

        Grid {
            id: row
            rows: root.vertical ? -1 : 1
            columns: root.vertical ? 1 : -1
            spacing: root.spacing
            add: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal }
                Eased { property: "scale"; from: 1 - 0.5 * Theme.motion; to: 1; duration: Theme.slow }
            }
            move: Transition { Eased { properties: "x,y"; duration: Theme.normal * Theme.motion } }
        }
    }
    Button {
        id: next
        objectName: "nextApps"
        visible: root.overflowing
        enabled: root.contentX < root.maximumX
        x: root.vertical ? 0 : parent.width - width
        y: root.vertical ? parent.height - height : 0
        width: root.vertical ? parent.width : root.navigationWidth
        height: root.vertical ? root.navigationWidth : parent.height
        subtle: true
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        icon.name: root.vertical ? "go-down" : "go-next"
        Accessible.name: root.nextLabel
        onClicked: root.scroll(1)
    }
}
