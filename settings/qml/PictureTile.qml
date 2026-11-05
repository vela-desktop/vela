// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
import Vela.Controls

// A choice shown by its picture, like Windows 11's themes: the picture on
// top, the name and a line under it; the one in use has the accent's border
// and a check. A click, Space or Enter chooses it (the gallery applies it
// at once).
// The picture goes in `picture`, filling pictureHeight.
T.AbstractButton {
    id: tile
    property bool selected: false
    property string subtitle
    property real pictureHeight: 120
    default property alias picture: pictureBox.data
    // In a gallery, Tab stops on the tile in use only (the gallery says
    // which), and on the one the arrows took the focus to while it has it;
    // the arrows ask the gallery to move the focus.
    property bool tabStop: true
    signal arrow(int dx, int dy)

    implicitWidth: 240
    implicitHeight: pictureHeight + 12 + labels.height + 12
    // With activeFocus: Qt Quick ignores a focus policy taken away while the
    // tile has the focus, so it is only ever taken away after.
    focusPolicy: tabStop || activeFocus ? Qt.TabFocus : Qt.NoFocus
    Keys.onLeftPressed: arrow(-1, 0)
    Keys.onRightPressed: arrow(1, 0)
    Keys.onUpPressed: arrow(0, -1)
    Keys.onDownPressed: arrow(0, 1)
    hoverEnabled: true
    Accessible.role: Accessible.RadioButton
    Accessible.checkable: true
    Accessible.checked: selected
    Accessible.description: subtitle
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    background: Fill {
        radius: Theme.radiusCard
        color: tile.down ? Theme.cardPressed : tile.hovered ? Theme.cardHover : Theme.card
        border.width: tile.selected ? 2 : 1
        border.color: tile.selected ? Theme.accentFill : Theme.cardStroke
        FocusRing { visible: tile.visualFocus }
    }

    contentItem: Item {
        Item {
            id: pictureBox
            x: 6
            y: 6
            width: parent.width - 12
            height: tile.pictureHeight
            clip: true
        }
        Column {
            id: labels
            x: 12
            y: pictureBox.y + pictureBox.height + 10
            width: parent.width - 24 - (tile.selected ? check.width + 8 : 0)
            spacing: 1
            Text {
                width: parent.width
                text: tile.text
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
            Text {
                // Its line even when empty: the tiles of a row stay as tall.
                width: parent.width
                text: tile.subtitle
                textFormat: Text.PlainText
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
        }
        // In use: a check in the accent, as Windows marks the theme in use.
        Rectangle {
            id: check
            visible: tile.selected
            anchors { right: parent.right; rightMargin: 12; top: labels.top; topMargin: 1 }
            width: 20
            height: 20
            radius: 10
            color: Theme.accentFill
            Text {
                anchors.centerIn: parent
                text: "✓"
                color: Theme.accentText
                font.pixelSize: 12
                font.weight: Font.Bold
            }
        }
    }
}
