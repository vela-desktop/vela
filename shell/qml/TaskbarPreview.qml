// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A taskbar button's previews, like Windows 11: with the mouse resting on an
// open app its windows (of the current desktop) appear above the button, with
// title and Close; a click goes there, a middle click closes it. If a window
// is part of a snap group, the group is there too: its windows in their
// places, and a click brings them all forward together.
Window {
    id: root
    visible: false
    width: panel.width + 32
    height: panel.height + 16
    color: "transparent"

    property var windows: [] // [{id, title, icon}]
    property var groups: [] // [{windows: [{id, title, icon, tile}]}]
    // The thumbnail version of each window (identifier -> n), once ready: it
    // changes at every capture, so the image reloads.
    property var ready: ({})

    readonly property int cardWidth: 208
    readonly property int cardHeight: 152

    Connections {
        target: Menus
        function onPreviewChanged() {
            if (Menus.preview) {
                root.show(Menus.preview)
            } else {
                root.hide()
            }
        }
        function onPreviewHoveredChanged() { root.checkHover() }
        function onPreviewButtonHoveredChanged() { root.checkHover() }
    }
    Connections {
        target: Capture
        function onThumbnailReady(identifier) {
            const next = Object.assign({}, root.ready)
            next[identifier] = (next[identifier] || 0) + 1
            root.ready = next
        }
    }
    // A window closed (or moved) while the previews are open.
    Connections {
        target: Desktops
        function onChanged() {
            if (root.visible && Menus.preview) root.collect(Menus.preview)
        }
    }

    // The mouse leaves button and previews: they close after a moment (moving
    // from one to the other doesn't close them).
    Timer {
        id: hideTimer
        interval: 300
        onTriggered: {
            if (!Menus.previewHovered && !Menus.previewButtonHovered) Menus.preview = null
        }
    }
    function checkHover() {
        if (!Menus.previewHovered && !Menus.previewButtonHovered) {
            hideTimer.restart()
        } else {
            hideTimer.stop()
        }
    }

    function collect(info) {
        const current = Desktops.current
        const list = Capture.windowList().filter(w => info.appIds.indexOf(w.appId) >= 0
            && (Desktops.workspaceOf(w.id) === current || Desktops.workspaceOf(w.id) === -1))
        windows = list.map(w => ({ id: w.id, title: w.title, icon: Apps.iconForAppId(w.appId) }))
        const ids = windows.map(w => w.id)
        const found = []
        for (const group of Desktops.snapGroups) {
            if (!group.windows.some(m => ids.indexOf(m.id) >= 0)) continue
            found.push({ windows: group.windows.map(m => ({
                id: m.id, tile: m.tile, title: Capture.title(m.id), icon: Apps.iconForAppId(Capture.appId(m.id))
            })) })
        }
        groups = found
        if (windows.length === 0) {
            Menus.preview = null
        }
    }

    function show(info) {
        collect(info)
        if (windows.length === 0) return
        const ids = windows.map(w => w.id)
        for (const g of groups) g.windows.forEach(m => { if (ids.indexOf(m.id) < 0) ids.push(m.id) })
        Capture.capture(ids)
        // On the taskbar's output, centered on the button.
        const screen = Qt.application.screens.find(s => s.name === info.screen)
        if (screen && screen !== root.screen) {
            visible = false
            Shell.placeOnScreen(root, screen.name)
        }
        const screenWidth = screen ? screen.width : Screen.width
        const w = (windows.length + groups.length) * (cardWidth + 4) + 12 + 32
        Shell.placeAtLeft(root, Math.round(Math.max(0, Math.min(screenWidth - w, info.center - w / 2))))
        if (!visible) {
            visible = true
            panel.opacity = 0
            slide.y = 8 * Theme.motion
            appear.restart()
        }
        updateBlur()
    }
    function hide() {
        hideTimer.stop()
        visible = false
        Menus.previewHovered = false
    }

    function updateBlur() {
        Effects.setBlur(root, [Qt.rect(panel.x, panel.y, panel.width, panel.height)])
    }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: panel; property: "opacity"; to: 1; duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        NumberAnimation { target: slide; property: "y"; to: 0; duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }

    // A card: icon, title, Close; the thumbnail (or the group) below.
    component Card: Rectangle {
        id: card
        property string title
        property string icon
        property bool closable: true
        default property alias preview: previewArea.data
        signal activated()
        signal closeRequested()
        width: root.cardWidth
        height: root.cardHeight
        radius: Theme.radiusOverlay
        color: cardHover.hovered ? Theme.subtleHover : "transparent"

        HoverHandler { id: cardHover }
        Row {
            id: header
            x: 10
            y: 8
            width: parent.width - 20 - (closable ? 24 : 0)
            spacing: 8
            Image {
                width: 16
                height: 16
                anchors.verticalCenter: parent.verticalCenter
                source: Theme.icons + encodeURIComponent(card.icon)
                sourceSize: Qt.size(width, height)
            }
            Text {
                width: header.width - 24
                anchors.verticalCenter: parent.verticalCenter
                text: card.title
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
        }
        Item {
            id: previewArea
            anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: parent.bottom; margins: 10; topMargin: 8 }
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            onClicked: mouse => {
                if (mouse.button === Qt.MiddleButton) {
                    if (card.closable) card.closeRequested()
                } else {
                    card.activated()
                }
            }
        }
        // Close, at the top right on hover.
        Rectangle {
            visible: card.closable && cardHover.hovered
            anchors { right: parent.right; top: parent.top; margins: 4 }
            width: 28
            height: 24
            radius: Theme.radius
            color: closeMouse.containsMouse ? "#c42b1c" : "transparent"
            Text {
                anchors.centerIn: parent
                text: "✕"
                color: closeMouse.containsMouse ? "white" : Theme.text
                font.pixelSize: 11
            }
            MouseArea {
                id: closeMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: card.closeRequested()
            }
        }
    }

    Shadow {
        target: panel
        opacity: panel.opacity
        blur: 24
        offset: Qt.vector2d(0, 4)
        radius: Theme.radiusOverlay
    }

    Rectangle {
        id: panel
        x: 16
        y: 4
        width: cards.width + 12
        height: root.cardHeight + 12
        radius: Theme.radiusOverlay
        color: Theme.surface
        border.width: 1
        border.color: Theme.stroke
        transform: Translate { id: slide }
        onWidthChanged: root.updateBlur()

        HoverHandler {
            onHoveredChanged: Menus.previewHovered = hovered
        }

        Row {
            id: cards
            x: 6
            y: 6
            spacing: 4

            Repeater {
                model: root.windows
                delegate: Card {
                    id: windowCard
                    required property var modelData
                    title: modelData.title
                    icon: modelData.icon
                    onActivated: {
                        Shell.windowAction(modelData.id, "activate")
                        Menus.preview = null
                    }
                    onCloseRequested: Shell.windowAction(modelData.id, "close")

                    Image {
                        id: thumbnail
                        anchors.fill: parent
                        fillMode: Image.PreserveAspectFit
                        source: root.ready[windowCard.modelData.id] ? "image://thumbnail/" + windowCard.modelData.id + "/" + root.ready[windowCard.modelData.id] : ""
                        cache: false
                        smooth: true
                    }
                    Image {
                        anchors.centerIn: parent
                        width: 40
                        height: 40
                        visible: thumbnail.status !== Image.Ready
                        source: Theme.icons + encodeURIComponent(windowCard.modelData.icon)
                        sourceSize: Qt.size(width, height)
                    }
                }
            }

            // Snap groups: the windows in their places, on a small output.
            Repeater {
                model: root.groups
                delegate: Card {
                    id: groupCard
                    required property var modelData
                    title: modelData.windows.map(m => m.title).join("  ·  ")
                    icon: modelData.windows.length > 0 ? modelData.windows[0].icon : ""
                    closable: false
                    onActivated: {
                        const ids = Menus.preview ? root.windows.map(w => w.id) : []
                        // The button's app window focused.
                        const mine = groupCard.modelData.windows.find(m => ids.indexOf(m.id) >= 0)
                        Shell.windowAction((mine || groupCard.modelData.windows[0]).id, "activate-group")
                        Menus.preview = null
                    }

                    Item {
                        id: screenShape
                        anchors.centerIn: parent
                        // The output's shape (16:9), inside the card's space.
                        width: Math.min(parent.width, parent.height * 16 / 9)
                        height: width * 9 / 16
                        Rectangle {
                            anchors.fill: parent
                            radius: 4
                            color: Theme.light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(1, 1, 1, 0.06)
                        }
                        Repeater {
                            model: groupCard.modelData.windows
                            delegate: Item {
                                id: member
                                required property var modelData
                                x: modelData.tile[0] / 12 * screenShape.width + 1
                                y: modelData.tile[1] / 12 * screenShape.height + 1
                                width: (modelData.tile[2] - modelData.tile[0]) / 12 * screenShape.width - 2
                                height: (modelData.tile[3] - modelData.tile[1]) / 12 * screenShape.height - 2
                                clip: true
                                Image {
                                    id: memberThumb
                                    anchors.fill: parent
                                    fillMode: Image.PreserveAspectCrop
                                    source: root.ready[member.modelData.id] ? "image://thumbnail/" + member.modelData.id + "/" + root.ready[member.modelData.id] : ""
                                    cache: false
                                    smooth: true
                                }
                                Image {
                                    anchors.centerIn: parent
                                    width: 20
                                    height: 20
                                    visible: memberThumb.status !== Image.Ready
                                    source: Theme.icons + encodeURIComponent(member.modelData.icon)
                                    sourceSize: Qt.size(width, height)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
