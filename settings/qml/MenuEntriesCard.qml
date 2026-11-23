// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// One context menu in Personalization > Context menu: a card saying how many
// of its entries show, that opens (a click, or Space or Enter) a switch for
// each one under it, like StyleChooser. A switch turned off hides the entry
// (Prefs.hiddenMenuEntries, docs/renderer.md §14.12); an entry may stand for
// several keys (a group of the jump list).
Column {
    id: card
    property string icon
    property string title
    // What it is about, before the count.
    property string about
    // [{ keys: ["desktop.view"], text: "View" }]
    property var entries: []
    property bool expanded: false
    readonly property int hiddenCount: {
        const hidden = Prefs.hiddenMenuEntries
        return entries.filter(e => e.keys.some(k => hidden.indexOf(k) >= 0)).length
    }

    width: parent ? parent.width : 0

    Card {
        id: header
        objectName: "menuHeader"
        icon: card.icon
        title: card.title
        description: card.hiddenCount === 0
            ? qsTr("%1. All %n entries shown", "", card.entries.length).arg(card.about)
            : qsTr("%1. %n hidden", "", card.hiddenCount).arg(card.about)
        clickable: true
        last: !card.expanded
        trailing: Chevron {
            anchors.verticalCenter: parent.verticalCenter
            down: true
            rotation: card.expanded ? 180 : 0
            color: Theme.textSecondary
        }
        onClicked: card.expanded = !card.expanded
    }

    Fill {
        objectName: "menuBody"
        visible: card.expanded
        width: parent.width
        height: visible ? rows.height + 16 : 0
        color: Theme.card
        radius: 0
        bottomLeftRadius: Theme.radiusCard
        bottomRightRadius: Theme.radiusCard
        border.width: 1
        border.color: Theme.cardStroke

        Column {
            id: rows
            x: 0
            y: 8
            width: parent.width

            Repeater {
                model: card.expanded ? card.entries : []
                delegate: Item {
                    id: row
                    required property var modelData
                    readonly property bool shown: !modelData.keys.some(k => Prefs.hiddenMenuEntries.indexOf(k) >= 0)
                    width: rows.width
                    height: 44

                    Icon {
                        id: entryIcon
                        x: 58
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        visible: !!row.modelData.icon
                        name: row.modelData.icon || ""
                    }
                    Text {
                        x: 58 + (entryIcon.visible ? 28 : 0)
                        width: toggle.x - x - 16
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.text
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    Switch {
                        id: toggle
                        objectName: "entry:" + row.modelData.keys[0]
                        anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
                        checked: row.shown
                        Accessible.name: row.modelData.text
                        onToggled: {
                            Prefs.setMenuEntriesHidden(row.modelData.keys, !checked)
                            // A save that failed leaves row.shown as it was,
                            // and the binding wouldn't move the switch back.
                            checked = Qt.binding(() => row.shown)
                        }
                    }
                }
            }
        }
    }
}
