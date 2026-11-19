// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

Window {
    id: root
    visible: false
    color: "transparent"
    width: panel.width + 2 * shadow.reach
    height: panel.height + shadow.reach + 12

    readonly property var info: Menus.trayOverflow
    readonly property var entries: info && info.bar ? info.bar.hiddenTrayIcons : []
    property int selected: 0
    property int left: 0
    onEntriesChanged: {
        if (entries.length === 0 && info) Menus.trayOverflow = null
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
    function place() {
        if (!info || entries.length === 0) return
        left = Math.round(Math.max(0, Math.min(Screen.width - width, info.center - width / 2)))
        Shell.placeAtLeft(root, left)
        visible = true
        panel.forceActiveFocus()
        motion.open()
    }
    onActiveChanged: if (!active && info && !Menus.isOpen) Menus.trayOverflow = null

    ShadowRoom {
        panels: [panel]
        onOutside: Menus.trayOverflow = null
    }
    PanelMotion {
        id: motion
        x: shadow.reach
        y: shadow.reach
        width: 172
        height: grid.height + 12
        offset: Qt.point(0, 8)
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

            Grid {
                id: grid
                x: 6
                y: 6
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
                        width: 40
                        height: 36
                        highlighted: root.active && index === root.selected
                        onInvoked: button => {
                            root.selected = index
                            const anchor = root.left + icon.mapToItem(null, icon.width / 2, 0).x
                            if (button === Qt.RightButton) {
                                Tray.requestMenu(entry.row, anchor)
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
