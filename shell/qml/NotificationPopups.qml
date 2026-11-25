// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Notifications, at the bottom right above the taskbar like on Windows 11. The
// window is as tall as the tiles: the rest of the output stays clickable.
// Each tile is a NotificationToast.
Window {
    id: root
    width: 380
    height: Math.max(1, stack.implicitHeight + 24)
    // C++ controls visibility so the layer can remap without losing notifications.
    visible: false
    color: "transparent"

    // The blurred background under the tiles: the whole window, the tiles give
    // the shape (outside is transparent).
    function updateBlur() {
        Effects.setBlur(root, [Qt.rect(0, 0, width, height)])
    }
    onWidthChanged: updateBlur()
    onHeightChanged: updateBlur()
    Component.onCompleted: updateBlur()

    // Mouse over: they don't expire while being read.
    HoverHandler {
        onHoveredChanged: Notifications.setHovered(hovered)
    }

    Column {
        id: stack
        x: 12
        y: 12
        width: parent.width - 24
        spacing: 8

        Repeater {
            model: Notifications

            delegate: NotificationToast {
                width: stack.width
                onInvoked: key => Notifications.invoke(notificationId, key)
                onDismissed: Notifications.dismiss(notificationId)
                onLinkActivated: link => System.openUrl(link)
            }
        }
    }
}
