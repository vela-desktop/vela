// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Windows 11 Snap Assist: as soon as a window snaps, the other windows appear
// as previews in the spaces left free; a click snaps the chosen one there, and
// the next space follows. Esc or a click elsewhere closes. Spaces are in
// twelfths of the output's usable area (the window covers exactly that: the
// taskbar stays out).
Window {
    id: root
    visible: false
    color: "transparent"

    property var zones: [] // the spaces to fill, in order
    property int current: 0 // the one whose windows are being chosen
    property var candidates: [] // [{id, title, icon}]
    property int revision: 0
    property string origin: "" // the snapped window it started from

    function overlaps(a, b) {
        return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3]
    }

    // The free spaces: those of the chosen layout (if the snap comes from
    // layouts), otherwise the complement of a half or a quarter.
    function freeZones(state) {
        const tile = state.tile
        let zones
        if (Menus.snapLayout && Menus.snapLayout.window === state.window) {
            zones = Menus.snapLayout.zones
        } else if (tile[1] === 0 && tile[3] === 12) {
            zones = [[0, 0, 6, 12], [6, 0, 12, 12]]
        } else if (tile[2] - tile[0] === 6 && tile[3] - tile[1] === 6) {
            zones = [[0, 0, 6, 6], [6, 0, 12, 6], [0, 6, 6, 12], [6, 6, 12, 12]]
        } else {
            zones = []
        }
        Menus.snapLayout = null
        return zones.filter(z => !overlaps(z, tile) && !state.occupied.some(o => overlaps(z, o)))
    }

    Connections {
        target: Shell
        function onSnapAssistRequested(json) {
            const state = JSON.parse(json)
            const zones = root.freeZones(state)
            if (zones.length === 0 || state.candidates.length === 0) {
                return
            }
            const screen = Qt.application.screens.find(s => s.name === state.output)
            if (screen && screen !== root.screen) {
                root.visible = false
                Shell.placeOnScreen(root, screen.name)
            }
            root.zones = zones
            root.origin = state.window
            root.current = 0
            root.candidates = state.candidates.map(id => ({
                id: id, title: Capture.title(id), icon: Apps.iconForAppId(Capture.appId(id))
            }))
            Capture.capture(state.candidates)
            root.visible = true
            content.opacity = 0
            appear.restart()
            content.forceActiveFocus()
            root.updateBlur()
        }
    }
    Connections {
        target: Capture
        function onThumbnailReady(identifier) { root.revision++ }
    }

    function close() {
        visible = false
        zones = []
    }
    function pick(id) {
        const z = zones[current]
        // Next to the window just snapped: together they form a snap group.
        Shell.windowAction(id, "snap " + z.join(" ") + " quiet " + origin)
        candidates = candidates.filter(c => c.id !== id)
        if (current + 1 < zones.length && candidates.length > 0) {
            current++
            updateBlur()
        } else {
            close()
        }
    }
    function zoneRect(z) {
        return Qt.rect(Math.round(width * z[0] / 12), Math.round(height * z[1] / 12),
            Math.round(width * z[2] / 12) - Math.round(width * z[0] / 12),
            Math.round(height * z[3] / 12) - Math.round(height * z[1] / 12))
    }
    function updateBlur() {
        if (visible) {
            Effects.setBlur(root, zones.slice(current).map(z => {
                const r = zoneRect(z)
                return Qt.rect(r.x + 4, r.y + 4, r.width - 8, r.height - 8)
            }))
        }
    }
    onWidthChanged: updateBlur()
    onHeightChanged: updateBlur()
    onActiveChanged: if (!active && visible && !Menus.isOpen) close()

    NumberAnimation { id: appear; target: content; property: "opacity"; to: 1; duration: Theme.normal }

    Item {
        id: content
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: root.close()

        MouseArea {
            anchors.fill: parent
            onClicked: root.close()
        }

        Repeater {
            model: root.zones
            delegate: Rectangle {
                id: zone
                required property var modelData
                required property int index
                readonly property rect area: root.zoneRect(modelData)
                readonly property bool active: index === root.current
                visible: index >= root.current
                x: area.x + 4
                y: area.y + 4
                width: area.width - 8
                height: area.height - 8
                radius: Theme.radiusOverlay
                color: Theme.light ? (active ? Qt.rgba(0.94, 0.94, 0.96, 0.78) : Qt.rgba(0.94, 0.94, 0.96, 0.5))
                    : (active ? Qt.rgba(0.12, 0.12, 0.14, 0.72) : Qt.rgba(0.12, 0.12, 0.14, 0.45))
                border.width: 1
                border.color: Theme.stroke

                MouseArea { anchors.fill: parent } // inside a space it doesn't close

                // The windows to offer, as previews, in the active space.
                Item {
                    id: space
                    visible: zone.active
                    anchors { fill: parent; margins: 24 }
                    // As many columns as needed to fill the space with 16:10
                    // cards.
                    readonly property int columns: Math.max(1, Math.min(root.candidates.length,
                        Math.ceil(Math.sqrt(root.candidates.length * width / Math.max(1, height) / 1.4))))
                    readonly property int rows: Math.ceil(root.candidates.length / columns)
                    readonly property real cardWidth: Math.min(320, (width - 16 * (columns - 1)) / columns,
                        ((height - 16 * (rows - 1)) / rows - 34) / 0.62)
                Grid {
                    anchors.centerIn: parent
                    columns: space.columns
                    spacing: 16

                    Repeater {
                        model: root.candidates
                        delegate: Fill {
                            id: card
                            required property var modelData
                            width: space.cardWidth
                            height: width * 0.62 + 34
                            radius: Theme.radiusOverlay
                            color: cardMouse.containsMouse ? Theme.taskCardHover : Theme.taskCard
                            border.width: cardMouse.containsMouse ? 2 : 1
                            border.color: cardMouse.containsMouse ? Theme.accent : Theme.stroke
                            Behavior on border.color { ColorFade {} }
                            Row {
                                x: 10
                                y: 8
                                width: parent.width - 20
                                spacing: 8
                                Icon {
                                    name: card.modelData.icon
                                }
                                Text {
                                    width: parent.width - 24
                                    text: card.modelData.title
                                    color: Theme.text
                                    font.pixelSize: Theme.fontCaption
                                    elide: Text.ElideRight
                                }
                            }
                            Image {
                                x: 8
                                y: 32
                                width: parent.width - 16
                                height: parent.height - 40
                                source: root.revision > 0 ? "image://thumbnail/" + card.modelData.id + "/" + root.revision : ""
                                fillMode: Image.PreserveAspectFit
                                cache: false
                            }
                            MouseArea {
                                id: cardMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: root.pick(card.modelData.id)
                            }
                        }
                    }
                }
                }
            }
        }
    }
}
