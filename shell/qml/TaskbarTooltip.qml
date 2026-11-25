// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A taskbar tooltip, like Windows 11's: a small flyout above the taskbar,
// centered on what the mouse rests on (Menus.tooltip). The mouse goes
// through it. It fades in and goes at once (docs/visual-coherence.md K5).
// Expressive: M3's plain tooltip, inverse surface with its text, extra small
// corners, 8 on each side and 24 high for a line.
Window {
    id: root
    readonly property bool expressive: Theme.expressive
    visible: false
    // The tooltip with room for its shadow around it, cut where the window
    // meets the taskbar (docs/visual-coherence.md §6.9): it rests 8 units
    // from it, on whichever edge it is.
    readonly property real room: shadow.reach
    readonly property QtObject bar: Styles.taskbar
    width: bar.pad("left", 0, 8, room) + panel.width + bar.pad("right", 0, 8, room)
    height: bar.pad("top", 0, 8, room) + panel.height + bar.pad("bottom", 0, 8, room)
    color: "transparent"

    readonly property string text: Menus.tooltip ? Menus.tooltip.text : ""

    Connections {
        target: Menus
        function onTooltipChanged() {
            if (Menus.tooltip) {
                root.show(Menus.tooltip)
            } else {
                motion.close()
            }
        }
    }

    function show(info) {
        // On the taskbar's output, centered on the item.
        const screen = Qt.application.screens.find(s => s.name === info.screen)
        if (screen && screen !== root.screen) {
            visible = false
            Shell.placeOnScreen(root, screen.name)
        }
        Menus.placeBesideBar(root, info.center, screen)
        visible = true
        motion.open()
    }

    PanelMotion {
        id: motion
        x: root.bar.pad("left", 0, 8, root.room)
        y: root.bar.pad("top", 0, 8, root.room)
        width: label.width + 2 * label.x
        height: label.height + 2 * label.y
        instantExit: true
        onClosed: root.visible = false

        Shadow {
            id: shadow
            target: panel
            radius: panel.radius
        }

        Rectangle {
            id: panel
            objectName: "tooltip"
            anchors.fill: parent
            radius: root.expressive ? Theme.shapeExtraSmall : Theme.radius
            color: root.expressive ? Theme.inverseSurface : Theme.popup
            border.width: root.expressive ? 0 : 1
            border.color: Theme.stroke

            Text {
                id: label
                x: root.expressive ? 8 : 10
                // Expressive: a caption line (15) and 4.5 above and below
                // make M3's 24.
                y: root.expressive ? 4 : 7
                width: Math.min(implicitWidth, 360)
                text: root.text
                color: root.expressive ? Theme.inverseSurfaceText : Theme.text
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }
        }
    }
}
