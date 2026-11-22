// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

Window {
    id: root
    visible: false
    color: "transparent"
    // The panel with room for its shadow, 12 from the taskbar on whichever
    // edge it is (docs/taskbar.md §11.3).
    readonly property QtObject bar: Styles.taskbar
    width: bar.pad("left", 0, 12, shadow.reach) + panel.width + bar.pad("right", 0, 12, shadow.reach)
    height: bar.pad("top", 0, 12, shadow.reach) + panel.height + bar.pad("bottom", 0, 12, shadow.reach)

    readonly property var info: Menus.trayOverflow
    readonly property var entries: info && info.bar ? info.bar.hiddenTrayIcons : []
    property int selected: 0
    property int left: 0 // its start along the bar, on the output
    // The rows that fit on the screen beside the taskbar; the others scroll.
    readonly property int gridRoom: Math.max(36, Screen.height - (bar.vertical ? 0 : bar.thickness) - shadow.reach - 24)
    onSelectedChanged: {
        const top = Math.floor(selected / grid.columns) * 36
        if (top < viewport.contentY) viewport.contentY = top
        else if (top + 36 > viewport.contentY + viewport.height) viewport.contentY = top + 36 - viewport.height
    }
    onEntriesChanged: {
        // Empty, it closes: later, not while `entries` itself is changing.
        if (entries.length === 0 && info) Qt.callLater(() => { if (root.entries.length === 0) Menus.trayOverflow = null })
        selected = Math.max(0, Math.min(selected, entries.length - 1))
    }
    onInfoChanged: {
        if (!info) {
            motion.close()
            return
        }
        selected = 0
        Shell.placeOnScreen(root, info.screen)
        Qt.callLater(root.place)
    }
    // This window's top-left on the output, against the bar.
    function origin() {
        const thick = info && info.bar ? (bar.vertical ? info.bar.width : info.bar.height) : bar.thickness
        return bar.position === "top" ? Qt.point(left, thick)
            : bar.position === "left" ? Qt.point(thick, left)
            : bar.position === "right" ? Qt.point(Screen.width - thick - width, left)
            : Qt.point(left, Screen.height - thick - height)
    }
    // From this window's coordinates to the taskbar's.
    function toBar(p) {
        const o = origin()
        const b = info.bar.barOrigin()
        return Qt.point(p.x + o.x - b.x, p.y + o.y - b.y)
    }
    // An icon's middle along the bar, on the output: its menu opens there.
    function centerOnScreen(item) {
        const o = origin()
        const p = item.mapToItem(null, item.width / 2, item.height / 2)
        return bar.vertical ? o.y + p.y : o.x + p.x
    }
    function place() {
        if (!info || entries.length === 0) return
        left = Menus.placeBesideBar(root, info.center, root.screen)
        visible = true
        panel.forceActiveFocus()
        motion.open()
    }
    // More icons hidden while it is open: it grows along a vertical bar, and
    // moves up to stay on the screen.
    onHeightChanged: if (visible && info && bar.vertical) left = Menus.placeBesideBar(root, info.center, root.screen)
    onActiveChanged: if (!active && info && !Menus.isOpen) Menus.trayOverflow = null

    ShadowRoom {
        panels: [panel]
        onOutside: Menus.trayOverflow = null
    }
    PanelMotion {
        id: motion
        x: root.bar.pad("left", 0, 12, shadow.reach)
        y: root.bar.pad("top", 0, 12, shadow.reach)
        width: 172
        height: viewport.height + 12
        offset: root.bar.towards(8)
        duration: Theme.normal
        instantExit: true
        onClosed: root.visible = false
        onVisibleAreaChanged: Effects.setBlur(root, [visibleArea], Theme.expressive ? Theme.radiusOverlay : 0)

        Shadow { id: shadow; target: panel }
        Rectangle {
            id: panel
            anchors.fill: parent
            radius: Theme.radiusOverlay
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke

            Keys.onPressed: event => {
                let next = root.selected
                if (event.key === Qt.Key_Escape) {
                    Menus.trayOverflow = null
                } else if (event.key === Qt.Key_Left) {
                    next -= 1
                } else if (event.key === Qt.Key_Right) {
                    next += 1
                } else if (event.key === Qt.Key_Up) {
                    next -= 4
                } else if (event.key === Qt.Key_Down) {
                    next += 4
                } else if (event.key === Qt.Key_Enter || event.key === Qt.Key_Return) {
                    const icon = icons.itemAt(root.selected)
                    if (icon) icon.invoked(Qt.LeftButton)
                } else {
                    return
                }
                root.selected = Math.max(0, Math.min(next, root.entries.length - 1))
                event.accepted = true
            }

            Flickable {
                id: viewport
                x: 6
                y: 6
                width: grid.width
                height: Math.min(grid.height, root.gridRoom)
                contentHeight: grid.height
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                clip: true

                Grid {
                    id: grid
                    columns: 4
                    Repeater {
                        id: icons
                        model: root.entries
                        delegate: TrayIcon {
                            id: icon
                            required property int index
                            required property var modelData
                            objectName: "trayOverflowIcon-" + index
                            entry: modelData
                            centerOnScreen: () => root.centerOnScreen(icon)
                            width: 40
                            height: 36
                            highlighted: root.active && index === root.selected
                            // Out onto the taskbar (docs/taskbar.md §8.2).
                            draggable: entry.key !== ""
                            onDragMoved: p => {
                                const q = root.toBar(p)
                                root.info.bar.trayDragMoved(entry.key, q.x, q.y, true)
                            }
                            onDropped: p => {
                                const q = root.toBar(p)
                                root.info.bar.trayDropped(entry.key, q.x, q.y, true)
                            }
                            onDragCanceled: root.info.bar.trayDragCanceled()
                            onInvoked: button => {
                                root.selected = index
                                const anchor = root.centerOnScreen(icon)
                                if (button === Qt.RightButton) {
                                    Tray.requestMenu(entry.row, anchor, true)
                                } else {
                                    if (button === Qt.MiddleButton) Tray.secondaryActivate(entry.row)
                                    else Tray.activate(entry.row, anchor)
                                    Menus.trayOverflow = null
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
