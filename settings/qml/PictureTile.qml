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
// Expressive: an M3 card's medium corners; the colors are the remapped
// card and accent tokens (primary for the one in use).
T.AbstractButton {
    id: tile
    property bool selected: false
    property string subtitle
    property real pictureHeight: 120
    default property alias picture: pictureBox.content
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
    // One that can't be chosen fades, like the other controls.
    opacity: enabled ? 1 : 0.45
    // Where the keyboard brings it, into view on a page that scrolls.
    onActiveFocusChanged: if (activeFocus) showInPage()
    function showInPage() {
        for (let item = parent; item; item = item.parent) {
            const page = item as Flickable
            if (page) {
                const top = mapToItem(page.contentItem, 0, 0).y
                if (top < page.contentY) {
                    page.contentY = Math.max(0, top - 8)
                } else if (top + height > page.contentY + page.height) {
                    page.contentY = Math.min(page.contentHeight - page.height, top + height - page.height + 8)
                }
                return
            }
        }
    }
    Keys.onLeftPressed: arrow(-1, 0)
    Keys.onRightPressed: arrow(1, 0)
    Keys.onUpPressed: arrow(0, -1)
    Keys.onDownPressed: arrow(0, 1)
    hoverEnabled: true
    // Qt's own check state is what a screen reader hears (Qt's button writes
    // it over the attached Accessible one once accessibility is on): the
    // tile is checked while selected and never toggles itself, the gallery
    // chooses.
    checkable: true
    checked: selected
    onToggled: checked = Qt.binding(() => tile.selected)
    Accessible.role: Accessible.RadioButton
    Accessible.description: subtitle
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    background: Fill {
        radius: Theme.expressive ? Theme.shapeMedium : Theme.radiusCard
        color: tile.down ? Theme.cardPressed : tile.hovered ? Theme.cardHover : Theme.card
        border.width: tile.selected ? 2 : 1
        border.color: tile.selected ? Theme.accentFill : Theme.cardStroke
        FocusRing { visible: tile.visualFocus }
    }

    contentItem: Item {
        // The picture, 6 inside the tile: its corners are the tile's less
        // that (Theme.nested), square in a tile no rounder than 6.
        RoundClip {
            id: pictureBox
            x: 6
            y: 6
            width: parent.width - 12
            height: tile.pictureHeight
            radius: Theme.nested(tile.background.radius, 6, 0)
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
