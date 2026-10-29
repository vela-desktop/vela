// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Snipping Tool on one output (one per output, created by snip.cpp): the
// darkened photo of the output, and at the top the mode bar like Windows 11:
// Rectangle, Window, Full screen. Drag a rectangle, or click a window or the
// output; Esc cancels.
Window {
    id: root
    property string screenName
    visible: false
    color: "black"

    // The chosen rectangle (or the window under the mouse), in this output's
    // coordinates.
    property rect selection: Qt.rect(0, 0, 0, 0)
    property point origin
    property bool dragging: false
    // Text, like Rectangle, drags a rectangle; it copies the text in it.
    // Color mode: the color under the mouse, shown beside it.
    property string hoverColor: ""
    property point hoverAt
    readonly property bool dragMode: Snip.mode === "rect" || Snip.mode === "text" || Snip.mode === "record"
    readonly property bool hasSelection: selection.width > 0 && selection.height > 0

    // The Text mode: the chosen rectangle stays, with the words found in it
    // (like Windows' Text Actions) to select and copy. textState: "" (choosing),
    // "busy", "words", "empty" or "failed".
    property string textState: ""
    property string textError: ""
    property var words: []
    property rect textRect: Qt.rect(0, 0, 0, 0)
    property int selFrom: -1
    property int selTo: -1
    property point menuAt
    property bool menuOpen: false
    readonly property int selMin: Math.min(selFrom, selTo)
    readonly property int selMax: Math.max(selFrom, selTo)

    function resetText() {
        textState = ""
        words = []
        selFrom = -1
        selTo = -1
        menuOpen = false
        selection = Qt.rect(0, 0, 0, 0)
    }
    function beginText(r) {
        if (r.width < 8 || r.height < 8) {
            return
        }
        textRect = r
        textState = "busy"
        Snip.recognize(root.screenName, r.x, r.y, r.width, r.height)
    }
    // The word under the point, else the nearest one.
    function wordAt(x, y) {
        let best = -1
        let bestDistance = Infinity
        for (let i = 0; i < words.length; ++i) {
            const w = words[i]
            const dx = Math.max(w.x - x, 0, x - (w.x + w.w))
            const dy = Math.max(w.y - y, 0, y - (w.y + w.h))
            const distance = dx * dx + dy * dy * 4
            if (distance < bestDistance) {
                best = i
                bestDistance = distance
            }
        }
        return best
    }
    function textOf(from, to) {
        let text = ""
        for (let i = from; i <= to; ++i) {
            if (i > from) {
                text += words[i].line !== words[i - 1].line ? "\n" : " "
            }
            text += words[i].text
        }
        return text
    }
    function copySelected() {
        if (selFrom >= 0) {
            Snip.copyText(textOf(selMin, selMax))
        }
    }
    function copyAll() {
        if (words.length > 0) {
            Snip.copyText(textOf(0, words.length - 1))
        }
    }
    function selectAll() {
        if (words.length > 0) {
            selFrom = 0
            selTo = words.length - 1
        }
    }

    Connections {
        target: Snip
        function onWordsReady(screen, found) {
            if (screen !== root.screenName || root.textState !== "busy") return
            root.words = found
            root.selFrom = -1
            root.selTo = -1
            root.textState = found.length > 0 ? "words" : "empty"
        }
        function onRecognizeFailed(screen, reason) {
            if (screen !== root.screenName || root.textState !== "busy") return
            root.textError = reason
            root.textState = "failed"
        }
    }

    // The topmost window under the point (this output's coordinates).
    function windowAt(x, y) {
        const gx = x + Screen.virtualX
        const gy = y + Screen.virtualY
        for (const w of Snip.windows) {
            if (gx >= w.x && gx < w.x + w.w && gy >= w.y && gy < w.y + w.h) {
                // Only the part on this output.
                const x0 = Math.max(w.x - Screen.virtualX, 0)
                const y0 = Math.max(w.y - Screen.virtualY, 0)
                const x1 = Math.min(w.x + w.w - Screen.virtualX, root.width)
                const y1 = Math.min(w.y + w.h - Screen.virtualY, root.height)
                return Qt.rect(x0, y0, x1 - x0, y1 - y0)
            }
        }
        return Qt.rect(0, 0, 0, 0)
    }
    function finish(r) {
        if (r.width >= 2 && r.height >= 2) {
            Snip.finish(root.screenName, r.x, r.y, r.width, r.height)
        }
    }

    Image {
        anchors.fill: parent
        source: root.screenName !== "" ? "image://snip/" + root.screenName : ""
        cache: false
        smooth: true
    }

    // The veil, except where the choice is being made.
    readonly property color veil: Qt.rgba(0, 0, 0, 0.45)
    Rectangle { color: root.veil; x: 0; y: 0; width: parent.width; height: root.hasSelection ? root.selection.y : parent.height }
    Rectangle {
        visible: root.hasSelection
        color: root.veil
        x: 0; y: root.selection.y + root.selection.height
        width: parent.width; height: parent.height - y
    }
    Rectangle {
        visible: root.hasSelection
        color: root.veil
        x: 0; y: root.selection.y; width: root.selection.x; height: root.selection.height
    }
    Rectangle {
        visible: root.hasSelection
        color: root.veil
        x: root.selection.x + root.selection.width; y: root.selection.y
        width: parent.width - x; height: root.selection.height
    }
    Rectangle {
        visible: root.hasSelection
        x: root.selection.x - 1
        y: root.selection.y - 1
        width: root.selection.width + 2
        height: root.selection.height + 2
        color: "transparent"
        border.width: 1
        border.color: "white"
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.CrossCursor
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: mouse => {
            if (mouse.button === Qt.RightButton) {
                Snip.cancel()
                return
            }
            if (Snip.mode === "color") {
                Snip.pickColor(root.screenName, mouse.x, mouse.y)
                return
            }
            if (root.dragMode) {
                root.origin = Qt.point(mouse.x, mouse.y)
                root.dragging = true
                root.selection = Qt.rect(mouse.x, mouse.y, 0, 0)
            }
        }
        onPositionChanged: mouse => {
            if (Snip.mode === "color") {
                root.hoverAt = Qt.point(mouse.x, mouse.y)
                root.hoverColor = Snip.colorAt(root.screenName, mouse.x, mouse.y)
            } else if (root.dragMode && root.dragging) {
                root.selection = Qt.rect(Math.min(root.origin.x, mouse.x), Math.min(root.origin.y, mouse.y),
                    Math.abs(mouse.x - root.origin.x), Math.abs(mouse.y - root.origin.y))
            } else if (Snip.mode === "window") {
                root.selection = root.windowAt(mouse.x, mouse.y)
            } else if (Snip.mode === "screen") {
                root.selection = Qt.rect(0, 0, root.width, root.height)
            }
        }
        onExited: if (!root.dragMode) root.selection = Qt.rect(0, 0, 0, 0)
        onReleased: mouse => {
            if (mouse.button !== Qt.LeftButton || Snip.mode === "color") return
            if (root.dragMode) {
                root.dragging = false
                if (Snip.mode === "text") {
                    root.beginText(root.selection)
                } else if (Snip.mode === "record" && root.selection.width < 6 && root.selection.height < 6) {
                    root.finish(Qt.rect(0, 0, root.width, root.height)) // a click: the whole output
                } else {
                    root.finish(root.selection)
                }
            } else if (Snip.mode === "window") {
                root.finish(root.windowAt(mouse.x, mouse.y))
            } else {
                root.finish(Qt.rect(0, 0, root.width, root.height))
            }
        }
    }

    // The words, and the mouse once the rectangle is chosen.
    Item {
        visible: root.textState !== ""
        anchors.fill: parent
        Rectangle {
            x: root.textRect.x; y: root.textRect.y; width: root.textRect.width; height: root.textRect.height
            color: Qt.rgba(0, 0, 0, 0.25)
        }
        Repeater {
            model: root.words
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool picked: root.selFrom >= 0 && index >= root.selMin && index <= root.selMax
                x: modelData.x - 2
                y: modelData.y - 1
                width: modelData.w + 4
                height: modelData.h + 2
                radius: 3
                color: picked ? Qt.rgba(0.25, 0.5, 0.95, 0.55) : Qt.rgba(1, 1, 1, 0.3)
            }
        }
        Text {
            visible: root.textState !== "words"
            x: root.textRect.x + (root.textRect.width - width) / 2
            y: root.textRect.y + (root.textRect.height - height) / 2
            text: root.textState === "busy" ? qsTr("Reading text…")
                : root.textState === "empty" ? qsTr("No text found") : root.textError
            color: "white"
            font.pixelSize: Theme.fontBody
            style: Text.Outline
            styleColor: "black"
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.IBeamCursor
            property bool selecting: false
            onPressed: mouse => {
                root.menuOpen = false
                const r = root.textRect
                const inside = mouse.x >= r.x && mouse.x < r.x + r.width && mouse.y >= r.y && mouse.y < r.y + r.height
                if (!inside) {
                    root.resetText() // choose another rectangle
                    return
                }
                if (mouse.button === Qt.RightButton) {
                    root.menuAt = Qt.point(mouse.x, mouse.y)
                    root.menuOpen = root.textState === "words"
                    return
                }
                if (root.textState === "words") {
                    selecting = true
                    root.selFrom = root.wordAt(mouse.x, mouse.y)
                    root.selTo = root.selFrom
                }
            }
            onPositionChanged: mouse => {
                if (selecting) root.selTo = root.wordAt(mouse.x, mouse.y)
            }
            onReleased: mouse => {
                if (selecting && root.selFrom >= 0) {
                    root.menuAt = Qt.point(mouse.x, mouse.y) // like Windows: the menu comes with the selection
                    root.menuOpen = true
                }
                selecting = false
            }
        }
        Rectangle {
            visible: root.menuOpen
            x: Math.min(root.menuAt.x, root.width - width - 8)
            y: Math.min(root.menuAt.y, root.height - height - 8)
            width: 220
            height: menuItems.height + 8
            radius: Theme.radius
            color: Theme.dialog
            border.width: 1
            border.color: Theme.stroke
            Column {
                id: menuItems
                x: 4; y: 4
                width: parent.width - 8
                Repeater {
                    model: [
                        { label: qsTr("Copy text"), key: "Ctrl+C", copy: true },
                        { label: qsTr("Select all"), key: "Ctrl+A", copy: false }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        width: parent.width
                        height: 34
                        radius: Theme.radius
                        color: itemMouse.containsMouse ? Theme.subtleHover : "transparent"
                        Text { x: 10; anchors.verticalCenter: parent.verticalCenter; text: modelData.label; color: Theme.text; font.pixelSize: Theme.fontBody }
                        Text { anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter } text: modelData.key; color: Theme.text; opacity: 0.6; font.pixelSize: Theme.fontBody }
                        MouseArea {
                            id: itemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                root.menuOpen = false
                                if (modelData.copy) root.copySelected(); else root.selectAll()
                            }
                        }
                    }
                }
            }
        }
    }

    // Color mode: the hex of the pixel, with a swatch, near the mouse.
    Rectangle {
        visible: Snip.mode === "color" && root.hoverColor !== ""
        x: Math.min(root.hoverAt.x + 18, root.width - width - 8)
        y: Math.min(root.hoverAt.y + 18, root.height - height - 8)
        width: swatchText.width + 44
        height: 30
        radius: Theme.radius
        color: Theme.dialog
        border.width: 1
        border.color: Theme.stroke
        Rectangle { x: 6; anchors.verticalCenter: parent.verticalCenter; width: 18; height: 18; radius: 3; color: root.hoverColor; border.width: 1; border.color: Theme.stroke }
        Text { id: swatchText; x: 32; anchors.verticalCenter: parent.verticalCenter; text: root.hoverColor; color: Theme.text; font.pixelSize: Theme.fontBody }
    }

    // The mode bar, at the top center.
    Rectangle {
        id: toolbar
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 24 }
        width: tools.width + 16
        height: 48
        radius: Theme.radiusOverlay
        color: Theme.dialog
        border.width: 1
        border.color: Theme.stroke

        Row {
            id: tools
            anchors.centerIn: parent
            spacing: 4
            Repeater {
                model: [
                    { mode: "rect", icon: "select-rectangular", tip: qsTr("Rectangle") },
                    { mode: "window", icon: "window", tip: qsTr("Window") },
                    { mode: "screen", icon: "view-fullscreen", tip: qsTr("Full screen") },
                    { mode: "text", icon: "edit-select-text", tip: qsTr("Text") },
                    { mode: "color", icon: "color-picker", tip: qsTr("Color picker") },
                    { mode: "record", icon: "media-record", tip: qsTr("Record") }
                ]
                delegate: Rectangle {
                    id: tool
                    required property var modelData
                    readonly property bool current: Snip.mode === modelData.mode
                    width: 40
                    height: 36
                    radius: Theme.radius
                    color: current ? Theme.subtleHover : toolMouse.containsMouse ? Theme.subtleHover : "transparent"
                    border.width: current ? 1 : 0
                    border.color: Theme.accent
                    Image {
                        anchors.centerIn: parent
                        width: 20
                        height: 20
                        source: Theme.icons + tool.modelData.icon
                        sourceSize: Qt.size(width, height)
                    }
                    MouseArea {
                        id: toolMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            Snip.mode = tool.modelData.mode
                            root.resetText()
                        }
                    }
                }
            }
            Rectangle {
                visible: root.textState === "words"
                width: copyAllText.width + 24
                height: 36
                radius: Theme.radius
                color: copyAllMouse.containsMouse ? Theme.subtleHover : "transparent"
                Text {
                    id: copyAllText
                    anchors.centerIn: parent
                    text: qsTr("Copy all text")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                MouseArea { id: copyAllMouse; anchors.fill: parent; hoverEnabled: true; onClicked: root.copyAll() }
            }
            // Record mode: drag an area or click for the whole output; sound off or microphone.
            Rectangle {
                visible: Snip.mode === "record"
                width: micText.width + 24
                height: 36
                radius: Theme.radius
                color: micMouse.containsMouse ? Theme.subtleHover : "transparent"
                Text {
                    id: micText
                    anchors.centerIn: parent
                    text: Snip.microphone ? qsTr("Sound: microphone") : qsTr("Sound: off")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                MouseArea { id: micMouse; anchors.fill: parent; hoverEnabled: true; onClicked: Snip.toggleMicrophone() }
            }
            Rectangle { width: 1; height: 24; anchors.verticalCenter: parent.verticalCenter; color: Theme.stroke }
            // The delay: click to cycle None, 3, 5, 10 seconds.
            Rectangle {
                width: delayText.width + 24
                height: 36
                radius: Theme.radius
                color: delayMouse.containsMouse ? Theme.subtleHover : "transparent"
                Text {
                    id: delayText
                    anchors.centerIn: parent
                    text: Snip.delay === 0 ? qsTr("Delay: none") : qsTr("Delay: %1 s").arg(Snip.delay)
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                MouseArea { id: delayMouse; anchors.fill: parent; hoverEnabled: true; onClicked: Snip.cycleDelay() }
            }
            Rectangle { width: 1; height: 24; anchors.verticalCenter: parent.verticalCenter; color: Theme.stroke }
            Rectangle {
                width: 40
                height: 36
                radius: Theme.radius
                color: closeMouse.containsMouse ? Theme.subtleHover : "transparent"
                Text {
                    anchors.centerIn: parent
                    text: "✕"
                    color: Theme.text
                    font.pixelSize: 13
                }
                MouseArea {
                    id: closeMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Snip.cancel()
                }
            }
        }
    }

    Item {
        focus: true
        Keys.onEscapePressed: Snip.cancel()
        Keys.onPressed: event => {
            if (root.textState !== "") {
                if (event.matches(StandardKey.Copy)) root.copySelected()
                else if (event.matches(StandardKey.SelectAll)) root.selectAll()
                return
            }
            // Like Windows: Enter with a rectangle chosen confirms it.
            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && root.hasSelection) {
                if (Snip.mode === "text") root.beginText(root.selection); else root.finish(root.selection)
            }
        }
    }
}
