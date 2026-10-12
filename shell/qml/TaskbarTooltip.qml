// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A taskbar tooltip, like Windows 11's: a small flyout above the taskbar,
// centered on what the mouse rests on (Menus.tooltip). The mouse goes
// through it.
Window {
    id: root
    visible: false
    width: panel.width + 16
    height: panel.height + 8
    color: "transparent"

    readonly property string text: Menus.tooltip ? Menus.tooltip.text : ""

    Connections {
        target: Menus
        function onTooltipChanged() {
            if (Menus.tooltip) {
                root.show(Menus.tooltip)
            } else {
                root.visible = false
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
        const screenWidth = screen ? screen.width : Screen.width
        Shell.placeAtLeft(root, Math.round(Math.max(0, Math.min(screenWidth - width, info.center - width / 2))))
        visible = true
    }

    Rectangle {
        id: panel
        x: 8
        width: label.width + 20
        height: label.height + 14
        radius: Theme.radius
        color: Theme.popup
        border.width: 1
        border.color: Theme.stroke

        Text {
            id: label
            x: 10
            y: 7
            width: Math.min(implicitWidth, 360)
            text: root.text
            color: Theme.text
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            lineHeight: 1.15
        }
    }
}
