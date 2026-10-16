// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Time & language > Keyboard: the layouts (Win+Space switches between them),
// and key repeat.
Page {
    id: page
    readonly property var layouts: Prefs.keyboardLayouts

    CardGroup {
        title: qsTr("Keyboard layout")
        Card {
            icon: "input-keyboard"
            title: page.layouts.length === 0 ? qsTr("The system's") : qsTr("Installed layouts")
            description: page.layouts.length > 1 ? qsTr("Win+Space to switch between layouts") : page.layouts.length === 0 ? qsTr("Chosen by localectl or KDE") : ""
            trailing: Button {
                text: qsTr("Add a keyboard")
                onClicked: {
                    layoutSearch.text = ""
                    addDialog.open()
                    layoutSearch.forceActiveFocus()
                }
            }
        }
        Repeater {
            model: page.layouts
            delegate: Card {
                id: layoutCard
                required property var modelData
                required property int index
                icon: "input-keyboard"
                title: Layouts.name(modelData.layout, modelData.variant)
                description: index === 0 ? qsTr("Default") : ""
                trailing: Row {
                    spacing: 4
                    Button {
                        visible: layoutCard.index > 0
                        subtle: true
                        text: qsTr("Move up")
                        onClicked: Prefs.moveKeyboardLayoutUp(layoutCard.index)
                    }
                    Button {
                        subtle: true
                        text: qsTr("Remove")
                        onClicked: Prefs.removeKeyboardLayout(layoutCard.index)
                    }
                }
            }
        }
    }

    CardGroup {
        title: qsTr("Key repeat")
        Card {
            icon: "chronometer"
            title: qsTr("Repeat delay")
            description: qsTr("How long to hold a key before it repeats: ") + Prefs.repeatDelay + " ms"
            trailing: Slider {
                width: 220
                from: 150
                to: 1000
                stepSize: 50
                live: false
                value: Prefs.repeatDelay
                onReleased: value => Prefs.repeatDelay = Math.round(value)
            }
        }
        Card {
            icon: "chronometer"
            title: qsTr("Repeat rate")
            description: Prefs.repeatRate + qsTr(" characters per second")
            trailing: Slider {
                width: 220
                from: 5
                to: 60
                stepSize: 1
                live: false
                value: Prefs.repeatRate
                onReleased: value => Prefs.repeatRate = Math.round(value)
            }
        }
        Card {
            icon: "edit-entry"
            title: qsTr("Test")
            trailing: TextBox {
                width: 260
                placeholderText: qsTr("Hold a key down here")
            }
        }
    }

    Dialog {
        id: addDialog
        parent: root.contentItem
        title: qsTr("Add a keyboard")
        buttons: [{ text: qsTr("Cancel"), value: "cancel" }]
        dialogWidth: 520
        readonly property var all: visible ? Layouts.all() : []
        readonly property var matches: {
            const q = layoutSearch.text.trim().toLowerCase()
            const list = q === "" ? all : all.filter(l => l.name.toLowerCase().indexOf(q) >= 0 || l.layout === q)
            return list.slice(0, 200)
        }
        Column {
            width: parent.width
            spacing: 8
            TextBox {
                id: layoutSearch
                width: parent.width
                placeholderText: qsTr("Search (e.g. Italian, English)")
            }
            ListView {
                id: layoutList
                width: parent.width
                height: 320
                clip: true
                model: addDialog.matches
                boundsBehavior: Flickable.StopAtBounds
                delegate: Rectangle {
                    id: layoutEntry
                    required property var modelData
                    width: layoutList.width
                    height: 36
                    radius: Theme.radius
                    color: entryMouse.containsMouse ? Theme.subtleHover : "transparent"
                    Text {
                        x: 12
                        width: parent.width - 24
                        anchors.verticalCenter: parent.verticalCenter
                        text: layoutEntry.modelData.name + "   " + layoutEntry.modelData.layout + (layoutEntry.modelData.variant !== "" ? "(" + layoutEntry.modelData.variant + ")" : "")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        id: entryMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            Prefs.addKeyboardLayout(layoutEntry.modelData.layout, layoutEntry.modelData.variant)
                            addDialog.close()
                        }
                    }
                }
            }
        }
    }
}
