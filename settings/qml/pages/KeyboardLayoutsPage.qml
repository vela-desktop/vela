// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes
import Vela.Controls

// Keyboard > Layouts: the installed layouts (Win+Space switches between
// them), where the layout in use comes from when it isn't one of them
// (docs/keyboard.md §5.5), and the dialog that adds one after trying it
// (§5.1-5.4).
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
                onClicked: addDialog.openToAdd()
            }
        }
        Repeater {
            model: page.layouts
            delegate: Card {
                id: layoutCard
                required property var modelData
                required property int index
                icon: Layouts.flag(modelData.layout, modelData.variant) || "input-keyboard"
                title: Layouts.name(modelData.layout, modelData.variant, Layouts.revision)
                description: index === 0 ? qsTr("Default") : ""
                trailing: Row {
                    spacing: 4
                    Button {
                        objectName: "preview:" + layoutCard.modelData.layout
                        subtle: true
                        text: qsTr("Preview")
                        onClicked: addDialog.openToPreview(layoutCard.modelData.layout, layoutCard.modelData.variant)
                    }
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

    // The list and search above, the chosen entry's preview below: a
    // drawing of the keyboard and a field typed through it. Opened from an
    // installed layout's Preview, the preview alone.
    Dialog {
        id: addDialog
        objectName: "addLayoutDialog"
        parent: root.contentItem
        dialogWidth: 640
        // {layout, variant} of the entry tried, or null.
        property var selected: null
        property bool previewOnly: false
        readonly property bool addable: selected !== null && !page.full && !page.installed(selected.layout, selected.variant)
        title: previewOnly && selected ? Layouts.name(selected.layout, selected.variant, Layouts.revision) : qsTr("Add a keyboard")
        buttons: previewOnly ? [{ text: qsTr("Close"), value: "cancel" }]
            : [{ text: qsTr("Add"), accent: true, value: "add", enabled: addable }, { text: qsTr("Cancel"), value: "cancel" }]
        onChosen: value => {
            if (value === "add" && addable) {
                Prefs.addKeyboardLayout(selected.layout, selected.variant)
            }
        }
        // [{layout, name, flag, matches, variants: [{variant, name, flag}]}],
        // every layout the system has.
        readonly property var groups: visible && !previewOnly ? Layouts.groups(layoutSearch.text, Layouts.revision) : []
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
        function choose(layout, variant) {
            if (Preview.show(layout, variant, Prefs.keyboardOptions)) {
                selected = { layout: layout, variant: variant }
                tryField.text = ""
            }
        }
        function openToAdd() {
            previewOnly = false
            selected = null
            opened = ({})
            layoutSearch.text = ""
            open()
            layoutSearch.forceActiveFocus()
        }
        function openToPreview(layout, variant) {
            previewOnly = true
            selected = null
            choose(layout, variant)
            open()
            tryField.forceActiveFocus()
        }
        Column {
            width: parent.width
            spacing: 8
            TextBox {
                id: layoutSearch
                objectName: "layoutSearch"
                visible: !addDialog.previewOnly
                width: parent.width
                placeholderText: qsTr("Search by name, language or country")
            }
            Item {
                visible: !addDialog.previewOnly
                width: parent.width
                height: 200
                ListView {
                    id: layoutList
                    objectName: "layoutList"
                    anchors.fill: parent
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
                            flag: group.modelData.flag
                            count: group.modelData.variants.length
                            open: group.open
                            onExpandRequested: addDialog.toggle(group.modelData.layout)
                        }
                        Repeater {
                            model: group.open ? group.modelData.variants : []
                            delegate: LayoutRow {
                                required property var modelData
                                objectName: "layout:" + group.modelData.layout + "(" + modelData.variant + ")"
                                layout: group.modelData.layout
                                variant: modelData.variant
                                label: modelData.name
                                flag: modelData.flag
                                indent: 28
                            }
                        }
                    }
                }
                Text {
                    visible: layoutList.count === 0
                    anchors.centerIn: parent
                    text: qsTr("No layout found")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }
            }
            Text {
                visible: addDialog.selected === null
                width: parent.width
                height: 120
                text: qsTr("Choose a layout to see its keys and try it.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.Wrap
            }
            KeyboardDrawing {
                objectName: "keyboardDrawing"
                visible: addDialog.selected !== null
                width: parent.width
            }
            Row {
                visible: addDialog.selected !== null
                width: parent.width
                spacing: 8
                TryField {
                    id: tryField
                    width: parent.width - shapeChoice.width - parent.spacing
                }
                Choice {
                    id: shapeChoice
                    objectName: "shapeChoice"
                    width: 110
                    readonly property var shapes: ["iso", "ansi", "jis", "abnt"]
                    model: ["ISO", "ANSI", "JIS", "ABNT2"]
                    currentIndex: shapes.indexOf(Preview.shape)
                    onChosen: index => Preview.shape = shapes[index]
                    Accessible.name: qsTr("Keyboard shape")
                }
            }
        }
    }

    // A layout or a variant in the dialog: a click tries it, a double click
    // adds it. A layout with variants has a button that shows them.
    component LayoutRow: ListItem {
        id: row
        property string layout
        property string variant
        property string label
        property string flag // a URL, or "": the generic icon
        property int count: 0
        property bool open: false
        property real indent: 0
        signal expandRequested()
        readonly property bool added: page.installed(layout, variant)
        width: parent ? parent.width : 0
        height: 40
        text: label
        highlighted: addDialog.selected !== null && addDialog.selected.layout === layout && addDialog.selected.variant === variant
        onClicked: addDialog.choose(layout, variant)
        onDoubleClicked: {
            addDialog.choose(layout, variant)
            if (addDialog.addable) {
                addDialog.close("add")
            }
        }
        Keys.onRightPressed: if (count > 0 && !open) expandRequested()
        Keys.onLeftPressed: if (open) expandRequested()
        contentItem: Item {
            Image {
                objectName: "flag"
                x: row.indent + 4
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 15
                visible: row.flag !== ""
                source: row.flag
                sourceSize.width: width
                fillMode: Image.PreserveAspectFit
            }
            Icon {
                x: row.indent + 4
                anchors.verticalCenter: parent.verticalCenter
                visible: row.flag === ""
                name: "input-keyboard"
                size: 20
            }
            Text {
                x: row.indent + 32
                width: parent.width - x - (expander.visible ? expander.width + 8 : 0) - addedLabel.width - 8
                anchors.verticalCenter: parent.verticalCenter
                text: row.label
                textFormat: Text.PlainText
                color: row.highlighted && !row.expressive ? "white" : row.added ? Theme.textSecondary : Theme.text
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
                onClicked: row.expandRequested()
                Chevron {
                    anchors.centerIn: parent
                    down: true
                    rotation: row.open ? 180 : 0
                }
            }
        }
    }

    // The chosen layout's keys (Preview.keys): on each, the characters of
    // its levels (Shift top left, AltGr bottom right, both top right), the
    // level it would type now in the text color, the others dimmed; the
    // keys held in the try field are lit.
    component KeyboardDrawing: Item {
        id: drawing
        readonly property real unit: width / Preview.columns
        height: unit * Preview.rows
        Repeater {
            model: Preview.keys
            delegate: Item {
                id: cap
                required property var modelData
                readonly property var labels: modelData.labels
                readonly property color fill: modelData.pressed ? Theme.accent : modelData.special ? Theme.subtleHover : Theme.control
                readonly property color ink: modelData.pressed ? "white" : Theme.text
                x: modelData.x * drawing.unit
                y: modelData.y * drawing.unit
                width: modelData.w * drawing.unit - 3
                height: modelData.h * drawing.unit - 3
                // The cap; a tall Enter is one L-shaped outline, its wider
                // top reaching left of its body.
                readonly property real reach: modelData.w2 > 0 ? (modelData.x2 - modelData.x) * drawing.unit : 0
                Rectangle {
                    visible: cap.modelData.w2 === 0
                    anchors.fill: parent
                    radius: 3
                    color: cap.fill
                    border.color: Theme.controlStroke
                }
                Shape {
                    visible: cap.modelData.w2 > 0
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        fillColor: cap.fill
                        strokeColor: Theme.controlStroke
                        strokeWidth: 1
                        joinStyle: ShapePath.RoundJoin
                        PathPolyline {
                            path: [
                                Qt.point(cap.reach + 0.5, 0.5),
                                Qt.point(cap.width - 0.5, 0.5),
                                Qt.point(cap.width - 0.5, cap.height - 0.5),
                                Qt.point(0.5, cap.height - 0.5),
                                Qt.point(0.5, drawing.unit - 3.5),
                                Qt.point(cap.reach + 0.5, drawing.unit - 3.5),
                                Qt.point(cap.reach + 0.5, 0.5)
                            ]
                        }
                    }
                }
                // A name (Shift, Tab), centered on the first row.
                Text {
                    visible: cap.modelData.special
                    x: cap.reach + 4
                    y: 3
                    width: drawing.unit * Math.max(cap.modelData.w, cap.modelData.w2) - 10
                    text: cap.labels[0]
                    color: cap.ink
                    font.pixelSize: Math.max(8, drawing.unit * 0.26)
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                }
                Glyph {
                    keyData: cap.modelData
                    unit: drawing.unit
                    ink: cap.ink
                    visible: !cap.modelData.special && !cap.modelData.letter
                    level: 0
                    x: 4
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 2
                    text: cap.labels[0]
                }
                Glyph {
                    keyData: cap.modelData
                    unit: drawing.unit
                    ink: cap.ink
                    visible: !cap.modelData.special
                    level: 1
                    x: 4
                    y: cap.modelData.letter ? (parent.height - height) / 2 - 2 : 1
                    // A letter: its capital, standing for both levels.
                    opacity: cap.modelData.letter || now || cap.modelData.pressed ? 1 : 0.55
                    text: cap.labels[1]
                }
                Glyph {
                    keyData: cap.modelData
                    unit: drawing.unit
                    visible: !cap.modelData.special && Preview.altGr
                    level: 2
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 2
                    text: cap.labels[2]
                    ink: cap.modelData.pressed ? "white" : Theme.accentText
                }
                Glyph {
                    keyData: cap.modelData
                    unit: drawing.unit
                    visible: !cap.modelData.special && Preview.altGr
                    level: 3
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    y: 1
                    text: cap.labels[3] !== cap.labels[2] ? cap.labels[3] : ""
                    ink: cap.modelData.pressed ? "white" : Theme.accentText
                }
            }
        }
    }

    // One level's character on a key of the drawing: the level it would
    // type now full, the others dimmed.
    component Glyph: Text {
        property var keyData
        property real unit
        property int level
        property color ink
        readonly property bool now: keyData.level === level
        font.pixelSize: Math.max(8, unit * (keyData.letter ? 0.42 : 0.32))
        color: ink
        opacity: now || keyData.pressed ? 1 : 0.55
        font.weight: now && !keyData.letter ? Font.DemiBold : Font.Normal
        textFormat: Text.PlainText
    }

    // A text box typed through the chosen layout, not the one in use: it
    // owns its key events and inserts what Preview.key() gives (§5.4).
    // Read-only, so Qt inserts nothing itself and no input method commits
    // into it. Tab, Enter and Esc go on to the dialog.
    component TryField: TextBox {
        id: field
        objectName: "tryField"
        readOnly: true
        selectByMouse: true
        placeholderText: qsTr("Type here to try the layout")
        // Replaces `start`..`end` (the selection by default) with `text`.
        function replaceSelection(text, start, end) {
            const from = start === undefined ? selectionStart : start
            const to = end === undefined ? selectionEnd : end
            field.text = field.text.slice(0, from) + text + field.text.slice(to)
            cursorPosition = from + text.length
        }
        function apply(result) {
            if (result.text !== "") {
                replaceSelection(result.text)
                return
            }
            switch (result.action) {
            case "backspace":
                if (selectedText !== "") {
                    replaceSelection("")
                } else if (cursorPosition > 0) {
                    replaceSelection("", cursorPosition - 1, cursorPosition)
                }
                break
            case "delete":
                if (selectedText !== "") {
                    replaceSelection("")
                } else if (cursorPosition < length) {
                    replaceSelection("", cursorPosition, cursorPosition + 1)
                }
                break
            case "left": cursorPosition = Math.max(0, cursorPosition - 1); break
            case "right": cursorPosition = Math.min(length, cursorPosition + 1); break
            case "home": cursorPosition = 0; break
            case "end": cursorPosition = length; break
            case "select-all": selectAll(); break
            case "copy": copy(); break
            case "cut": copy(); replaceSelection(""); break
            case "paste": replaceSelection(Preview.clipboardText().replace(/[\r\n]/g, " ")); break
            }
        }
        Keys.onPressed: event => {
            if ([Qt.Key_Tab, Qt.Key_Backtab, Qt.Key_Return, Qt.Key_Enter, Qt.Key_Escape].indexOf(event.key) >= 0) {
                event.accepted = false
                return
            }
            apply(Preview.key(event.nativeScanCode, true, event.isAutoRepeat))
            event.accepted = true
        }
        Keys.onReleased: event => {
            Preview.key(event.nativeScanCode, false, event.isAutoRepeat)
            event.accepted = true
        }
        onActiveFocusChanged: if (!activeFocus) Preview.reset()
        onCursorPositionChanged: caret.shown = true
        // Qt draws no cursor in a read-only field: this one blinks like it.
        Rectangle {
            id: caret
            property bool shown: true
            readonly property rect at: field.positionToRectangle(field.cursorPosition)
            visible: field.activeFocus && shown && field.selectedText === ""
            x: at.x
            y: at.y
            width: 1
            height: at.height
            color: field.color
            Timer {
                running: field.activeFocus
                repeat: true
                interval: Qt.styleHints.cursorFlashTime / 2
                onTriggered: caret.shown = !caret.shown
            }
        }
    }
}
