// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
import Vela.Controls

// A settings row, like Windows 11 "settings cards": icon, title and
// description on the left, the control on the right (the children). With
// `clickable` the whole card is a button (and shows the arrow if it leads to
// another page): for the pointer, the keyboard (Tab, then Space or Enter)
// and screen readers alike.
//
// Expressive: M3's filled card on surface container low, without an
// outline. In a CardGroup the cards are one container (M3's segmented
// list): large corners outside, extra small inside, 2 apart; `first` and
// `last` still cut a group (an expanding card's).
Fill {
    id: card
    readonly property bool expressive: Theme.expressive
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
    // Expressive: where it is in its CardGroup (the visible cards).
    readonly property bool grouped: expressive && parent !== null && parent.objectName === "cardGroupColumn"
    readonly property bool groupFirst: !grouped || neighbour(-1) === null
    readonly property bool groupLast: !grouped || neighbour(1) === null
    function neighbour(step) {
        const siblings = parent.children
        let i = 0
        while (i < siblings.length && siblings[i] !== card)
            ++i
        for (i += step; i >= 0 && i < siblings.length; i += step) {
            if (siblings[i].visible)
                return siblings[i]
        }
        return null
    }
    readonly property real outerRadius: expressive ? Theme.shapeLarge : Theme.radiusCard
    readonly property real innerRadius: expressive ? Theme.shapeExtraSmall : 0
    readonly property bool roundTop: first && groupFirst
    readonly property bool roundBottom: last && groupLast
    signal clicked()
    // A clickable card takes the keyboard's focus (a page shows it, asked).
    function takeFocus() {
        button.forceActiveFocus()
    }

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
    color: button.pressed && clickable ? Theme.cardPressed : button.hovered && clickable ? Theme.cardHover : Theme.card
    topLeftRadius: roundTop ? outerRadius : innerRadius
    topRightRadius: roundTop ? outerRadius : innerRadius
    bottomLeftRadius: roundBottom ? outerRadius : innerRadius
    bottomRightRadius: roundBottom ? outerRadius : innerRadius
    border.width: expressive ? 0 : 1
    border.color: Theme.cardStroke

    // The button under the card's controls (they come after it, on top).
    T.AbstractButton {
        id: button
        anchors.fill: parent
        enabled: card.clickable
        hoverEnabled: card.clickable
        focusPolicy: card.clickable ? Qt.TabFocus : Qt.NoFocus
        Accessible.role: Accessible.Button
        Accessible.name: card.title
        Accessible.description: card.description
        Accessible.ignored: !card.clickable
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        onClicked: card.clicked()
        HoverHandler {
            enabled: card.clickable
            cursorShape: Qt.PointingHandCursor
        }
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
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
            Text {
                visible: card.description !== ""
                width: parent.width
                text: card.description
                textFormat: Text.PlainText
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

    FocusRing {
        visible: button.visualFocus
        // Expressive: the card's own corners, 3 out.
        topLeftRadius: card.expressive ? card.topLeftRadius + 3 : radius
        topRightRadius: card.expressive ? card.topRightRadius + 3 : radius
        bottomLeftRadius: card.expressive ? card.bottomLeftRadius + 3 : radius
        bottomRightRadius: card.expressive ? card.bottomRightRadius + 3 : radius
    }
}
