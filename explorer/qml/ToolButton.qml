// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
import Vela.Controls

// A command bar or address bar button, like Windows 11: no background until
// hovered; an icon (`icon.name`), optional text, an arrow if it opens a menu;
// `checked` (a pane or a view that is on) rests on the hover color. Qt's tool
// button: a click, Space or Enter activate it; Tab gives it the focus and a
// ring, a click doesn't, so the keyboard stays with the view. Its color fades
// between states (Fill).
T.ToolButton {
    id: button
    property alias usable: button.enabled
    property bool menu: false // with the little arrow: opens a menu
    property string tooltip

    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: 32
    leftPadding: text !== "" ? 10 : 8
    rightPadding: leftPadding
    spacing: 8
    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    opacity: enabled ? 1 : 0.4
    icon.width: 16
    icon.height: 16
    Accessible.name: text !== "" ? text : tooltip

    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()
    onClicked: { tipTimer.stop(); tip.visible = false }
    onHoveredChanged: {
        if (hovered && tooltip !== "") {
            tipTimer.restart()
        } else {
            tipTimer.stop()
            tip.visible = false
        }
    }

    background: Fill {
        color: button.down ? Theme.subtlePressed : button.hovered || button.checked ? Theme.subtleHover : Theme.subtleRest
        FocusRing { visible: button.visualFocus }
    }

    contentItem: Item {
        implicitWidth: content.implicitWidth
        implicitHeight: content.implicitHeight
        Row {
            id: content
            anchors.centerIn: parent
            spacing: button.spacing
            Icon {
                visible: name !== ""
                anchors.verticalCenter: parent.verticalCenter
                name: button.icon.name
                size: button.icon.width
            }
            Text {
                visible: button.text !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: button.text
                color: Theme.text
                font.pixelSize: Theme.fontBody
            }
            Chevron {
                visible: button.menu
                anchors.verticalCenter: parent.verticalCenter
                down: true
                color: Theme.textSecondary
            }
        }
    }

    // The tooltip, like Windows: after resting on it a moment, below the
    // button.
    Timer {
        id: tipTimer
        interval: 600
        onTriggered: {
            const host = button.Window.window ? button.Window.window.contentItem : null
            if (!host) return
            tip.parent = host
            const p = button.mapToItem(host, button.width / 2, button.height + 6)
            tip.x = Math.max(4, Math.min(host.width - tip.width - 4, p.x - tip.width / 2))
            tip.y = p.y
            tip.visible = true
        }
    }
    Rectangle {
        id: tip
        objectName: "tooltip"
        visible: false
        z: 1000
        width: tipText.implicitWidth + 16
        height: tipText.implicitHeight + 10
        radius: Theme.radius
        color: Theme.flyout
        border.width: 1
        border.color: Theme.stroke
        Text {
            id: tipText
            anchors.centerIn: parent
            text: button.tooltip
            color: Theme.text
            font.pixelSize: Theme.fontCaption
        }
    }
}
