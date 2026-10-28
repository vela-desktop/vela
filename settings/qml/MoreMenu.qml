// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic as C
import Vela.Controls

// A card's "..." button and its menu, like Windows 11's: `entries` is
// [{text, action}]. Up and Down move between entries, Esc closes it.
Button {
    id: button
    property var entries: []

    subtle: true
    icon.name: "view-more-symbolic"
    Accessible.name: qsTr("More options")
    checked: menu.visible
    onClicked: menu.open()

    C.Popup {
        id: menu
        objectName: "menu"
        y: button.height + 2
        x: button.width - width
        padding: 4
        focus: true
        onOpened: list.children[0].forceActiveFocus()

        contentItem: Column {
            id: list
            Repeater {
                model: button.entries
                delegate: ListItem {
                    id: entry
                    required property var modelData
                    objectName: "entry:" + modelData.text
                    width: Math.max(180, label.implicitWidth + 24)
                    height: 36
                    text: modelData.text
                    Keys.onUpPressed: nextItemInFocusChain(false).forceActiveFocus()
                    Keys.onDownPressed: nextItemInFocusChain(true).forceActiveFocus()
                    onClicked: {
                        menu.close()
                        entry.modelData.action()
                    }
                    contentItem: Text {
                        id: label
                        leftPadding: 8
                        text: entry.text
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }
        background: Rectangle {
            color: Theme.flyout
            radius: Theme.radiusOverlay
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 0.4)
        }
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.fast } }
    }
}
