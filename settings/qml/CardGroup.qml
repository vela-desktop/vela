// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A group of cards with its title, like Windows 11 sections ("Scale & layout",
// "Related settings"...): cards slightly apart. Expressive: the cards are
// one container, 2 apart (M3's segmented gap; Card draws the corners), the
// title in the emphasis weight.
Column {
    id: group
    property string title
    default property alias cards: cardColumn.data
    width: parent ? parent.width : 0
    spacing: 0

    Text {
        visible: group.title !== ""
        text: group.title
        color: Theme.text
        font.pixelSize: Theme.fontBody
        font.weight: Theme.weightEmphasis
        topPadding: 20
        bottomPadding: 8
    }
    Column {
        id: cardColumn
        objectName: "cardGroupColumn"
        width: parent.width
        spacing: Theme.expressive ? 2 : 4
    }
}
