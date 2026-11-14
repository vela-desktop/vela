// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A question before an action that can't be undone (such as emptying the
// Recycle Bin), like Windows confirmation dialogs. Opened with
// Menus.confirm(); Enter confirms, Esc gives up.
// Expressive: M3's basic dialog, as Vela.Controls' Dialog: extra large
// corners, the headline in headline small, the supporting text in on
// surface variant, 24 around, "Yes" a tonal button at the trailing edge and
// "No" a text button before it. 200 high, Vela's choice for the larger
// headline.
Window {
    id: root
    visible: false
    readonly property bool expressive: Theme.expressive
    readonly property int panelHeight: expressive ? 200 : 170
    // The dialog with room for its shadow around it (docs/visual-coherence.md
    // §6.9).
    readonly property real room: shadow.reach
    width: room + 420 + room
    height: room + panelHeight + room
    color: "transparent"

    property string title
    property string text
    property string yesText: qsTr("Yes")
    property var action: null

    Connections {
        target: Menus
        function onConfirmRequested(title, text, yesText, action, screen) {
            Shell.placeOnScreen(root, screen || Shell.primaryScreen)
            root.title = title
            root.text = text
            root.yesText = yesText || qsTr("Yes")
            root.action = action
            root.visible = true
            motion.open()
            panel.forceActiveFocus()
        }
    }

    function answer(yes) {
        const run = yes ? action : null
        motion.close()
        action = null
        if (run) {
            Qt.callLater(run)
        }
    }

    onActiveChanged: {
        if (!active && motion.opened) {
            answer(false)
        }
    }

    // A click in the shadow's room says no, like a click outside.
    ShadowRoom {
        panels: [panel]
        onOutside: root.answer(false)
    }

    // It grows in from 1.05, like Windows 11's dialogs, and fades out.
    PanelMotion {
        id: motion
        x: root.room
        y: root.room
        width: 420
        height: root.panelHeight
        scaleFrom: 1.05
        duration: Theme.normal
        onClosed: root.visible = false

        Shadow {
            id: shadow
            target: panel
            depth: Theme.shadow.dialog
            radius: panel.radius
        }

        Rectangle {
            id: panel
            objectName: "confirmPanel"
            anchors.fill: parent
            radius: root.expressive ? Theme.shapeExtraLarge : Theme.radiusOverlay
            color: Theme.dialog
            border.width: root.expressive ? 0 : 1
            border.color: Theme.stroke
            focus: true
            Keys.onEscapePressed: root.answer(false)
            Keys.onReturnPressed: root.answer(true)
            Keys.onEnterPressed: root.answer(true)

            Text {
                id: titleText
                objectName: "confirmTitle"
                x: 24
                y: root.expressive ? 24 : 20
                width: root.expressive ? parent.width - 48 : implicitWidth
                text: root.title
                color: Theme.text
                font.pixelSize: root.expressive ? Math.round(Theme.fontBody * 24 / 14) : Theme.fontBody + 2
                font.weight: root.expressive ? Font.Normal : Font.DemiBold
                elide: root.expressive ? Text.ElideRight : Text.ElideNone
            }
            Text {
                objectName: "confirmText"
                x: 24
                // M3: 16 between the headline and the body.
                y: root.expressive ? titleText.y + titleText.height + 16 : 52
                width: parent.width - 48
                text: root.text
                color: root.expressive ? Theme.textSecondary : Theme.text
                font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
            }

            Row {
                anchors { right: parent.right; bottom: parent.bottom; margins: root.expressive ? 24 : 20 }
                spacing: 8
                // Expressive: "Yes" closest to the trailing edge.
                layoutDirection: root.expressive ? Qt.RightToLeft : Qt.LeftToRight

                Button {
                    objectName: "confirmYes"
                    text: root.yesText
                    accent: !root.expressive
                    onClicked: root.answer(true)
                }
                Button {
                    objectName: "confirmNo"
                    text: qsTr("No")
                    subtle: root.expressive
                    onClicked: root.answer(false)
                }
            }
        }
    }
}
