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
    readonly property bool full: layouts.length >= Layouts.maxLayouts
    function names(list) {
        return list.map(l => Layouts.name(l.layout, l.variant, Layouts.revision)).join(", ")
    }
    function installed(layout, variant) {
        return layouts.some(l => l.layout === layout && l.variant === variant)
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
                : page.full ? qsTr("Vela can switch between up to %1 layouts: remove one to add another.").arg(Layouts.maxLayouts)
                : page.layouts.length > 1 ? qsTr("Win+Space to switch between layouts")
                : page.layouts.length === 0 ? page.names(page.source.layouts)
                : ""
            trailing: Button {
                objectName: "addLayout"
                enabled: !page.full
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
                title: Layouts.name(modelData.layout, modelData.variant, Layouts.revision)
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
        objectName: "addLayoutDialog"
        parent: root.contentItem
        title: qsTr("Add a keyboard")
        buttons: [{ text: qsTr("Cancel"), value: "cancel" }]
        dialogWidth: 560
        // [{layout, name, matches, variants: [{variant, name}]}], every
        // layout the system has (docs/keyboard.md §5.1-5.2).
        readonly property var groups: visible ? Layouts.groups(layoutSearch.text, Layouts.revision) : []
        readonly property bool searching: layoutSearch.text.trim() !== ""
        // The groups opened by hand, by layout; a search opens those it
        // finds only through their variants.
        property var opened: ({})
        function isOpen(group) {
            return opened[group.layout] === true || (searching && !group.matches)
        }
        function toggle(layout) {
            const o = Object.assign({}, opened)
            o[layout] = !o[layout]
            opened = o
        }
        function add(layout, variant) {
            Prefs.addKeyboardLayout(layout, variant)
            close()
        }
        onVisibleChanged: if (visible) opened = ({})
        Column {
            width: parent.width
            spacing: 8
            TextBox {
                id: layoutSearch
                objectName: "layoutSearch"
                width: parent.width
                placeholderText: qsTr("Search by name, language or country")
            }
            ListView {
                id: layoutList
                objectName: "layoutList"
                width: parent.width
                height: 360
                clip: true
                model: addDialog.groups
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: false
                delegate: Column {
                    id: group
                    required property var modelData
                    readonly property bool open: addDialog.isOpen(modelData)
                    width: layoutList.width
                    LayoutRow {
                        objectName: "layout:" + group.modelData.layout
                        layout: group.modelData.layout
                        variant: ""
                        label: group.modelData.name
                        count: group.modelData.variants.length
                        open: group.open
                        onToggled: addDialog.toggle(group.modelData.layout)
                    }
                    Repeater {
                        model: group.open ? group.modelData.variants : []
                        delegate: LayoutRow {
                            required property var modelData
                            objectName: "layout:" + group.modelData.layout + "(" + modelData.variant + ")"
                            layout: group.modelData.layout
                            variant: modelData.variant
                            label: modelData.name
                            indent: 28
                        }
                    }
                }
            }
            Text {
                visible: layoutList.count === 0
                width: parent.width
                text: qsTr("No layout found")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    // A layout or a variant in the dialog: a click adds it, unless it's
    // installed already. A layout with variants has a button that shows
    // them.
    component LayoutRow: ListItem {
        id: row
        property string layout
        property string variant
        property string label
        property int count: 0
        property bool open: false
        property real indent: 0
        signal toggled()
        readonly property bool added: page.installed(layout, variant)
        width: parent ? parent.width : 0
        height: 40
        text: label
        // Not disabled when added: the button of its variants must work.
        onClicked: if (!added) addDialog.add(layout, variant)
        Keys.onRightPressed: if (count > 0 && !open) toggled()
        Keys.onLeftPressed: if (open) toggled()
        contentItem: Item {
            Text {
                x: row.indent + 4
                width: parent.width - x - (expander.visible ? expander.width + 8 : 0) - addedLabel.width - 8
                anchors.verticalCenter: parent.verticalCenter
                text: row.label
                textFormat: Text.PlainText
                color: row.added ? Theme.textSecondary : Theme.text
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
            Text {
                id: addedLabel
                anchors.right: expander.visible ? expander.left : parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: row.added
                text: visible ? qsTr("Added") : ""
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
            }
            Button {
                id: expander
                objectName: "expand:" + row.layout
                visible: row.count > 0
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                subtle: true
                width: 32
                height: 32
                Accessible.name: row.open ? qsTr("Hide variants") : qsTr("Show variants")
                onClicked: row.toggled()
                Chevron {
                    anchors.centerIn: parent
                    down: true
                    rotation: row.open ? 180 : 0
                }
            }
        }
    }
}
