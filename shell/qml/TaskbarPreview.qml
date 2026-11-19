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
    // The panel with room for its shadow around it, cut where the window
    // meets the taskbar (docs/visual-coherence.md §6.9): the panel rests 12
    // units from it, on whichever edge it is.
    readonly property real room: shadow.reach
    readonly property QtObject bar: Styles.taskbar
    width: bar.pad("left", 0, 12, room) + panel.width + bar.pad("right", 0, 12, room)
    height: bar.pad("top", 0, 12, room) + panel.height + bar.pad("bottom", 0, 12, room)
    color: "transparent"

    property int selected: 0
    property int outputWidth: Screen.width
    readonly property string attentionLabel: Menus.preview && Menus.preview.button && Menus.preview.button.attention
        ? Menus.preview.button.tooltip : ""
    onSelectedChanged: Qt.callLater(revealSelected)
    function revealSelected() {
        const left = selected * (cardWidth + cards.spacing)
        if (left < viewport.contentX) viewport.contentX = left
        else if (left + cardWidth > viewport.contentX + viewport.width)
            viewport.contentX = left + cardWidth - viewport.width
        viewport.returnToBounds()
    }
    readonly property bool latched: Menus.preview !== null && !!Menus.preview.latched
    onActiveChanged: if (!active && latched) Menus.preview = null
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
        function onAdvancePreview() {
            if (root.windows.length > 0) root.selected = (root.selected + 1) % root.windows.length
        }
        function onPreviewHoveredChanged() { root.checkHover() }
        function onPreviewButtonHoveredChanged() { root.checkHover() }
    }
    Connections {
        target: Shell
        function onSuperReleased() {
            if (Menus.preview && Menus.preview.cycling) {
                const card = windowCards.itemAt(root.selected)
                if (card) card.activated()
            }
        }
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

    Connections {
        target: Menus.preview ? Menus.preview.button : null
        function onWindowIdsChanged() {
            if (Menus.preview) root.collect(Menus.preview)
        }
    }

    // The mouse leaves button and previews: they close after a moment (moving
    // from one to the other doesn't close them).
    Timer {
        id: hideTimer
        interval: 300
        onTriggered: {
            if (!Menus.previewHovered && !Menus.previewButtonHovered && !root.latched) Menus.preview = null
        }
    }
    function checkHover() {
        if (latched) {
            hideTimer.stop()
        } else if (!Menus.previewHovered && !Menus.previewButtonHovered) {
            hideTimer.restart()
        } else {
            hideTimer.stop()
        }
    }

    function collect(info) {
        const idsToShow = info.button ? info.button.windowIds : []
        const captured = Capture.windowList()
        const list = Array.from(idsToShow).map(id => captured.find(w => w.id === id)).filter(w => w !== undefined)
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
        // Cycling with Super held picks only windows: on release, a group
        // card would activate nothing.
        const choices = info.cycling ? windows.length : windows.length + groups.length
        selected = Math.max(0, Math.min(selected, choices - 1))
        if (windows.length === 0) {
            Menus.preview = null
        }
    }

    function show(info) {
        hideTimer.stop()
        selected = 0
        viewport.contentX = 0
        collect(info)
        if (windows.length === 0) return
        if (info.cycling) {
            selected = (windows.findIndex(w => w.id === info.startingWindow) + 1) % windows.length
        }
        const ids = windows.map(w => w.id)
        for (const g of groups) g.windows.forEach(m => { if (ids.indexOf(m.id) < 0) ids.push(m.id) })
        Capture.capture(ids)
        // On the taskbar's output, centered on the button.
        const screen = Qt.application.screens.find(s => s.name === info.screen)
        if (screen && screen !== root.screen) {
            visible = false
            Shell.placeOnScreen(root, screen.name)
        }
        // Beside a vertical bar the cards have its width less.
        outputWidth = (screen ? screen.width : Screen.width) - (bar.vertical ? bar.thickness : 0)
        const count = windows.length + groups.length
        const panelWidth = Math.min(outputWidth, count * cardWidth + (count - 1) * cards.spacing + 12 + 2 * room)
        // Along a row its width, known before the cards are laid out.
        Menus.placeBesideBar(root, info.center, screen, bar.vertical ? undefined : panelWidth)
        Shell.setPanelKeyboard(root, info.latched)
        visible = true // again, on another output
        if (info.latched) panel.forceActiveFocus()
        motion.open()
        Effects.setBlur(root, [motion.visibleArea], Theme.expressive ? Theme.radiusOverlay : 0)
    }
    function hide() {
        hideTimer.stop()
        Menus.previewButtonHovered = false
        motion.close()
        Shell.setPanelKeyboard(root, false)
        Menus.previewHovered = false
    }

    // A card: icon, title, Close; the thumbnail (or the group) below.
    component Card: Fill {
        id: card
        property string title
        property string icon
        property bool closable: true
        property bool highlighted: false
        default property alias preview: previewArea.data
        signal activated()
        signal closeRequested()
        width: root.cardWidth
        height: root.cardHeight
        radius: Theme.nested(Theme.radiusOverlay, 6, Theme.radius) // 6 inside the panel
        color: card.highlighted || cardHover.hovered ? Theme.subtleHover : Theme.subtleRest

        HoverHandler { id: cardHover }
        Row {
            id: header
            x: 10
            y: 8
            width: parent.width - 20 - (closable ? 24 : 0)
            spacing: 8
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: card.icon
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
        Fill {
            visible: card.closable && cardHover.hovered
            anchors { right: parent.right; top: parent.top; margins: 4 }
            width: 28
            height: 24
            radius: Theme.nested(parent.radius, 4, Theme.radius) // 4 inside the card
            color: closeMouse.containsMouse ? "#c42b1c" : "#00c42b1c"
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

    // A click in the shadow's room closes the previews, like one outside.
    ShadowRoom {
        panels: [panel]
        onOutside: Menus.preview = null
    }

    // It rises 8 units, and goes at once (docs/visual-coherence.md K5).
    PanelMotion {
        id: motion
        x: root.bar.pad("left", 0, 12, root.room)
        y: root.bar.pad("top", 0, 12, root.room)
        width: Math.min(cards.width + 12, Math.max(12, root.outputWidth - root.room * 2))
        height: root.cardHeight + 12 + (attentionTitle.visible ? attentionTitle.height + 6 : 0)
        offset: root.bar.towards(8)
        duration: Theme.normal
        instantExit: true
        onClosed: root.visible = false
        // The travel while it moves, the panel at rest; round under the
        // expressive panel, as when it opens.
        onVisibleAreaChanged: Effects.setBlur(root, [visibleArea], Theme.expressive ? Theme.radiusOverlay : 0)

        Shadow {
            id: shadow
            target: panel
        }

        Rectangle {
            id: panel
            anchors.fill: parent
            radius: Theme.radiusOverlay
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke

            Keys.onPressed: event => {
                if (!root.latched) return
                const count = root.windows.length + root.groups.length
                const card = root.selected < root.windows.length
                    ? windowCards.itemAt(root.selected)
                    : groupCards.itemAt(root.selected - root.windows.length)
                if (event.key === Qt.Key_Escape) {
                    Menus.preview = null
                } else if (count > 0 && (event.key === Qt.Key_Left || event.key === Qt.Key_Up)) {
                    root.selected = (root.selected + count - 1) % count
                } else if (count > 0 && (event.key === Qt.Key_Right || event.key === Qt.Key_Down)) {
                    root.selected = (root.selected + 1) % count
                } else if (card && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                    card.activated()
                } else if (card && card.closable && event.key === Qt.Key_Delete) {
                    card.closeRequested()
                } else {
                    return
                }
                event.accepted = true
            }

            HoverHandler {
                onHoveredChanged: Menus.previewHovered = hovered
            }

            Text {
                id: attentionTitle
                visible: root.attentionLabel !== ""
                x: 10
                y: 6
                width: parent.width - 20
                text: root.attentionLabel
                elide: Text.ElideRight
                font.pixelSize: Theme.fontCaption
                color: Theme.text
            }
            Flickable {
                id: viewport
                x: 6
                y: attentionTitle.visible ? attentionTitle.y + attentionTitle.height + 6 : 6
                width: Math.max(0, parent.width - 12)
                height: root.cardHeight
                contentWidth: cards.width
                contentHeight: height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.HorizontalFlick
                interactive: contentWidth > width

                Row {
                    id: cards
                    spacing: 4

                    Repeater {
                        id: windowCards
                        model: root.windows
                        delegate: Card {
                            id: windowCard
                            required property int index
                            highlighted: root.latched && index === root.selected
                            required property var modelData
                            title: modelData.title
                            icon: modelData.icon
                            onActivated: {
                                Shell.windowAction(modelData.id, "activate")
                                Menus.preview = null
                            }
                            onCloseRequested: Shell.windowAction(modelData.id, "close")

                            DropArea {
                                anchors.fill: parent
                                enabled: !!(Menus.preview && Menus.preview.spring)
                                onEntered: {
                                    Menus.previewHovered = true
                                    springRaise.restart()
                                }
                                onExited: {
                                    Menus.previewHovered = false
                                    springRaise.stop()
                                }
                                onDropped: drop => {
                                    drop.accepted = false
                                    Menus.preview = null
                                }
                                Timer {
                                    id: springRaise
                                    interval: 600
                                    onTriggered: windowCard.activated()
                                }
                            }

                            Image {
                                id: thumbnail
                                anchors.fill: parent
                                fillMode: Image.PreserveAspectFit
                                source: root.ready[windowCard.modelData.id] ? "image://thumbnail/" + windowCard.modelData.id + "/" + root.ready[windowCard.modelData.id] : ""
                                cache: false
                                smooth: true
                            }
                            Icon {
                                anchors.centerIn: parent
                                visible: thumbnail.status !== Image.Ready
                                name: windowCard.modelData.icon
                                size: 40
                            }
                        }
                    }

                    // Snap groups: the windows in their places, on a small output.
                    Repeater {
                        id: groupCards
                        model: root.groups
                        delegate: Card {
                            id: groupCard
                            required property int index
                            highlighted: root.latched && index + root.windows.length === root.selected
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
                                        Icon {
                                            anchors.centerIn: parent
                                            visible: memberThumb.status !== Image.Ready
                                            name: member.modelData.icon
                                            size: 20
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
