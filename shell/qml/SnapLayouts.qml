// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Windows 11 snap layouts: hovering the Maximize button (or Win+Z) opens a
// panel below it with the possible arrangements; a click on a zone snaps the
// window there, and Snap Assist offers the other windows for the remaining
// zones. Zones are in twelfths of the usable area.
Window {
    id: root
    visible: false
    color: "transparent"

    property string window: ""
    property bool keyboard: false
    property real anchorX: 0
    property real anchorY: 0
    property int focusIndex: -1 // the zone chosen from the keyboard, among all of them

    readonly property var layouts: [
        [[0, 0, 6, 12], [6, 0, 12, 12]],
        [[0, 0, 8, 12], [8, 0, 12, 12]],
        [[0, 0, 4, 12], [4, 0, 8, 12], [8, 0, 12, 12]],
        [[0, 0, 6, 12], [6, 0, 12, 6], [6, 6, 12, 12]],
        [[0, 0, 6, 6], [6, 0, 12, 6], [0, 6, 6, 12], [6, 6, 12, 12]],
        [[0, 0, 3, 12], [3, 0, 9, 12], [9, 0, 12, 12]]
    ]
    // All zones one after the other (for the keyboard): [layout, zone].
    readonly property var flat: {
        const out = []
        for (let l = 0; l < layouts.length; ++l) {
            for (let z = 0; z < layouts[l].length; ++z) {
                out.push([l, z])
            }
        }
        return out
    }

    Connections {
        target: Shell
        function onSnapLayoutsRequested(window, output, x, y, keyboard) {
            const screen = Qt.application.screens.find(s => s.name === output)
            if (screen && screen !== root.screen) {
                root.visible = false
                Shell.placeOnScreen(root, screen.name)
            }
            root.window = window
            root.keyboard = keyboard
            root.anchorX = x
            root.anchorY = y
            root.focusIndex = keyboard ? 0 : -1
            root.visible = true
            motion.open()
            content.forceActiveFocus()
            leaveTimer.stop()
            Effects.setBlur(root, [motion.visibleArea])
        }
    }

    function close() {
        motion.close()
        leaveTimer.stop()
    }
    function choose(layoutIndex, zoneIndex) {
        const zones = layouts[layoutIndex]
        const z = zones[zoneIndex]
        Menus.snapLayout = { window: root.window, zones: zones }
        Shell.windowAction(root.window, "snap " + z.join(" "))
        close()
    }

    onActiveChanged: if (!active && motion.opened && !Menus.isOpen) close()

    // At the first opening the window's height comes later: the blurred zone
    // follows the panel's position (PanelMotion.visibleArea), not only the
    // window.
    readonly property real panelY: Math.min(anchorY + 4, height - panel.height - 8)

    // Leaving the panel (and the Maximize button, which stays below) closes
    // it, if opened with the mouse; a click outside always does.
    property point pointer: Qt.point(anchorX, anchorY - 16)
    function overButton(p) {
        return Math.abs(p.x - anchorX) <= 24 && p.y >= anchorY - 34 && p.y <= panelY
    }
    Timer {
        id: leaveTimer
        interval: 450
        onTriggered: if (!panelHover.hovered && !root.overButton(root.pointer)) root.close()
    }
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onClicked: root.close()
        onPositionChanged: mouse => {
            root.pointer = Qt.point(mouse.x, mouse.y)
            if (!root.keyboard && !panelHover.hovered && !root.overButton(root.pointer)) {
                if (!leaveTimer.running) leaveTimer.start()
            } else {
                leaveTimer.stop()
            }
        }
    }

    Item {
        id: content
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: root.close()
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Right || event.key === Qt.Key_Down || event.key === Qt.Key_Tab) {
                root.focusIndex = (root.focusIndex + 1) % root.flat.length
                event.accepted = true
            } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Up || event.key === Qt.Key_Backtab) {
                root.focusIndex = (root.focusIndex + root.flat.length - 1) % root.flat.length
                event.accepted = true
            } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) && root.focusIndex >= 0) {
                root.choose(root.flat[root.focusIndex][0], root.flat[root.focusIndex][1])
                event.accepted = true
            }
        }
    }

    // It comes down 6 units from the button, and leaves the same way.
    PanelMotion {
        id: motion
        x: Math.max(8, Math.min(root.width - width - 8, root.anchorX - width / 2))
        y: root.panelY
        width: grid.width + 24
        height: grid.height + 24
        offset: Qt.point(0, -6)
        duration: Theme.normal
        onClosed: root.visible = false
        onVisibleAreaChanged: Effects.setBlur(root, [visibleArea]) // the travel while it moves, the panel at rest

        Shadow {
            target: panel
        }

        Rectangle {
            id: panel
            anchors.fill: parent
            radius: Theme.radiusOverlay
            color: Theme.popup
            border.width: 1
            border.color: Theme.stroke

            HoverHandler {
                id: panelHover
                onHoveredChanged: if (hovered) leaveTimer.stop()
            }

            Grid {
                id: grid
                x: 12
                y: 12
                columns: 3
                spacing: 12

                Repeater {
                    model: root.layouts
                    delegate: Rectangle {
                        id: layout
                        required property var modelData
                        required property int index
                        width: 96
                        height: 60
                        radius: Theme.radius
                        color: Theme.light ? Qt.rgba(0, 0, 0, 0.03) : Qt.rgba(1, 1, 1, 0.05)
                        border.width: 1
                        border.color: Theme.stroke

                        Repeater {
                            model: layout.modelData
                            delegate: Rectangle {
                                id: zone
                                required property var modelData
                                required property int index
                                readonly property real unitX: (layout.width - 8) / 12
                                readonly property real unitY: (layout.height - 8) / 12
                                readonly property bool focused: root.focusIndex >= 0
                                    && root.flat[root.focusIndex][0] === layout.index && root.flat[root.focusIndex][1] === zone.index
                                x: 4 + modelData[0] * unitX + 2
                                y: 4 + modelData[1] * unitY + 2
                                width: (modelData[2] - modelData[0]) * unitX - 4
                                height: (modelData[3] - modelData[1]) * unitY - 4
                                radius: 3
                                color: zoneMouse.containsMouse || focused ? Theme.accent : Theme.light ? Qt.rgba(0, 0, 0, 0.16) : Qt.rgba(1, 1, 1, 0.22)
                                Behavior on color { ColorAnimation { duration: Theme.fast } }
                                MouseArea {
                                    id: zoneMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: root.choose(layout.index, zone.index)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
