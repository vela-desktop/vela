// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A settings row, like Windows 11 "settings cards": icon, title and
// description on the left, the control on the right (the children). With
// `clickable` the whole card is a button (and shows the arrow if it leads to
// another page).
Fill {
    id: card
    property string icon
    property string title
    property string description
    property bool clickable: false
    property bool chevron: false
    property bool first: true // rounded corners at the top
    property bool last: true // and at the bottom (in groups only the first and the last)
    property alias trailing: trailingRow.data
    property alias contentItem: extra.data // content below, full width
    property int minimumHeight: description !== "" ? 69 : 48
    signal clicked()

    width: parent ? parent.width : 0
    // The content below counts only when visible (cards that expand).
    readonly property bool hasExtra: {
        for (let i = 0; i < extra.children.length; ++i) {
            if (extra.children[i].visible) return true
        }
        return false
    }
    implicitHeight: (title !== "" || icon !== "" ? Math.max(minimumHeight, header.height + 24) : 16) + (hasExtra ? extra.height + 16 : 0)
    height: implicitHeight
    color: mouse.pressed && clickable ? Theme.cardPressed : mouse.containsMouse && clickable ? Theme.cardHover : Theme.card
    topLeftRadius: first ? Theme.radiusCard : 0
    topRightRadius: first ? Theme.radiusCard : 0
    bottomLeftRadius: last ? Theme.radiusCard : 0
    bottomRightRadius: last ? Theme.radiusCard : 0
    border.width: 1
    border.color: Theme.cardStroke

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: card.clickable
        enabled: card.clickable
        cursorShape: card.clickable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: card.clicked()
    }

    Item {
        id: header
        x: 16
        y: (Math.max(card.minimumHeight, height + 24) - height) / 2
        width: parent.width - 32
        height: card.title !== "" || card.icon !== "" ? Math.max(textColumn.height, trailingRow.height, 20) : 0

        Image {
            id: iconImage
            visible: card.icon !== ""
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            source: card.icon !== "" ? (card.icon.indexOf("://") >= 0 ? card.icon : Theme.icons + encodeURIComponent(card.icon)) : ""
            sourceSize: Qt.size(width, height)
        }
        Column {
            id: textColumn
            anchors.verticalCenter: parent.verticalCenter
            x: card.icon !== "" ? 36 : 0
            width: parent.width - x - trailingRow.width - (card.chevron ? 28 : 0) - 16
            spacing: 1
            Text {
                width: parent.width
                text: card.title
                color: Theme.text
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
            Text {
                visible: card.description !== ""
                width: parent.width
                text: card.description
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }
        }
        Row {
            id: trailingRow
            anchors { right: parent.right; rightMargin: card.chevron ? 28 : 0; verticalCenter: parent.verticalCenter }
            spacing: 8
        }
        Chevron {
            visible: card.chevron
            anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
            color: Theme.textSecondary
        }
    }

    Item {
        id: extra
        x: 16
        y: card.title !== "" || card.icon !== "" ? Math.max(card.minimumHeight, header.height + 24) : 16
        width: parent.width - 32
        height: childrenRect.height
    }
}
