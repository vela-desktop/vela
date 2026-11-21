// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Keyboard > Layouts: the installed layouts (Win+Space switches between
// them), and where the layout in use comes from when it isn't one of them
// (docs/keyboard.md §5.5).
Page {
    id: page
    readonly property var layouts: Prefs.keyboardLayouts
    // {source: "environment" | "system" | "default", layouts}: read once,
    // like the compositor reads it at startup.
    readonly property var source: Layouts.source()
    readonly property bool fromEnvironment: source.source === "environment"
    function names(list) {
        return list.map(l => Layouts.name(l.layout, l.variant)).join(", ")
    }

    CardGroup {
        title: qsTr("Keyboard layout")
        Card {
            objectName: "layoutSource"
            icon: "input-keyboard"
            title: page.fromEnvironment ? qsTr("Set by environment variables")
                : page.layouts.length > 0 ? qsTr("Installed layouts")
                : qsTr("From the system")
            description: page.fromEnvironment
                ? qsTr("XKB_DEFAULT_LAYOUT chooses %1. It applies instead of the layouts below until it is removed.").arg(page.names(page.source.layouts))
                : page.layouts.length > 1 ? qsTr("Win+Space to switch between layouts")
                : page.layouts.length === 0 ? page.names(page.source.layouts)
                : ""
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
                delegate: Fill {
                    id: layoutEntry
                    required property var modelData
                    width: layoutList.width
                    height: 36
                    radius: Theme.radius
                    color: entryMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
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
