// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Effects
import QtQuick.Shapes
import Vela.Controls

// One level of a menu (the menu or one of its submenus), like Windows 11
// menus: rounded corners, 32-high rows with the icon on the left and the
// shortcut on the right, highlight detached from the edges.
Item {
    id: panel

    property var entries: []
    property int level: 0
    // Where it starts: point (anchorX, anchorY); if there's no room on the
    // right it goes to altX (submenus then open to the left of the parent).
    property real anchorX: 0
    property real anchorY: 0
    property real altX: NaN
    property bool above: false // the menu rises from anchorY (such as from the taskbar)
    property bool centered: false // anchorX is the center
    property real minWidth: 0
    property bool keyboardMode: false
    property int currentIndex: -1
    property int parentIndex: -1 // the parent's entry that opened it
    // The shell's menus are surfaces the compositor blurs; an app's are drawn
    // inside its window (Files: Theme.flyout).
    property color fill: Theme.popup

    // The container (the whole output) to keep it within the edges.
    readonly property real areaWidth: parent ? parent.width : 0
    readonly property real areaHeight: parent ? parent.height : 0

    signal activated(var entry, int index)
    signal hovered(int index, real rowY)
    signal submenuRequested(int index, real rowY, bool fromKeyboard)
    signal contextRequested(var entry, real x, real y)
    signal backRequested() // left arrow or Esc
    signal closeAllRequested()
    signal pinToggled() // the entries must be rebuilt (a file was pinned or unpinned)

    // --- sizes ---
    readonly property int padding: 4
    readonly property int rowHeight: 32
    readonly property int separatorHeight: 9
    readonly property int headerHeight: 30
    readonly property int iconRowHeight: 44
    readonly property bool hasIcons: entries.some(e => !e.separator && !e.header && !e.iconRow && (e.icon || e.checked !== undefined))
    readonly property real textLeft: hasIcons ? 44 : 16

    FontMetrics {
        id: metrics
        font.pixelSize: Theme.fontBody
    }

    function plainText(text) {
        return (text || "").replace(/&(.)/g, "$1")
    }
    // The text with the keyboard letter underlined ("&&" is a real "&").
    function styledText(text) {
        const escape = t => t.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        let out = ""
        let underlined = false
        for (let i = 0; i < (text || "").length; ++i) {
            const c = text[i]
            if (c === "&" && i + 1 < text.length) {
                const next = text[++i]
                out += next === "&" || underlined ? escape(next) : "<u>" + escape(next) + "</u>"
                underlined = underlined || next !== "&"
            } else {
                out += escape(c)
            }
        }
        return out
    }
    function accessKey(entry) {
        const m = /(?:^|[^&])(?:&&)*&([^&])/.exec(entry.text || "")
        return (m ? m[1] : plainText(entry.text).charAt(0)).toLowerCase()
    }
    function rowHeightOf(entry) {
        return entry.separator ? separatorHeight : entry.header ? headerHeight : entry.iconRow ? iconRowHeight : rowHeight
    }
    function selectable(entry) {
        return entry && !entry.separator && !entry.header && !entry.iconRow && entry.enabled !== false
    }

    readonly property real contentWidth: {
        let widest = 0
        let shortcut = 0
        let extra = 0
        for (const e of entries) {
            if (e.separator || e.iconRow) {
                continue
            }
            widest = Math.max(widest, metrics.advanceWidth(plainText(e.header || e.text)))
            if (e.shortcut) {
                shortcut = Math.max(shortcut, metrics.advanceWidth(e.shortcut) + 32)
            }
            if (e.children || e.pin) {
                extra = 28
            }
        }
        return textLeft + widest + shortcut + extra + 20
    }
    readonly property real contentHeight: {
        let h = 2 * padding
        for (const e of entries) {
            h += rowHeightOf(e)
        }
        return h
    }

    width: Math.max(minWidth, Math.min(480, Math.ceil(contentWidth)))
    height: contentHeight
    x: {
        const margin = 8
        let left = centered ? anchorX - width / 2 : anchorX
        if (!centered && left + width > areaWidth - margin) {
            left = isNaN(altX) ? anchorX - width : altX - width
        }
        return Math.round(Math.max(margin, Math.min(areaWidth - width - margin, left)))
    }
    y: {
        const margin = 8
        let top = above ? anchorY - height : anchorY
        if (!above && top + height > areaHeight - margin) {
            top = level > 0 ? areaHeight - height - margin : anchorY - height
        }
        return Math.round(Math.max(margin, Math.min(areaHeight - height - margin, top)))
    }

    // It comes in fading and sliding slightly from the side it starts from.
    opacity: 0
    property real slide: (above ? 8 : -8) * Theme.motion
    transform: Translate { y: panel.slide }
    Component.onCompleted: appear.start()
    ParallelAnimation {
        id: appear
        NumberAnimation { target: panel; property: "opacity"; to: 1; duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        NumberAnimation { target: panel; property: "slide"; to: 0; duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }

    // --- keyboard ---
    function move(step) {
        const count = entries.length
        const start = currentIndex >= 0 ? currentIndex : (step > 0 ? -1 : count)
        for (let i = 1; i <= count; ++i) {
            const next = ((start + step * i) % count + count) % count
            if (selectable(entries[next])) {
                currentIndex = next
                return
            }
        }
    }
    function rowY(index) {
        let y = padding
        for (let i = 0; i < index; ++i) {
            y += rowHeightOf(entries[i])
        }
        return y
    }
    function trigger(index, fromKeyboard) {
        const entry = entries[index]
        if (!selectable(entry)) {
            return
        }
        if (entry.children) {
            submenuRequested(index, rowY(index), fromKeyboard)
        } else {
            activated(entry, index)
        }
    }

    focus: true
    Keys.onPressed: event => {
        switch (event.key) {
        case Qt.Key_Down: move(1); break
        case Qt.Key_Up: move(-1); break
        case Qt.Key_Home: currentIndex = -1; move(1); break
        case Qt.Key_End: currentIndex = 0; move(-1); break
        case Qt.Key_Right:
            if (currentIndex >= 0 && entries[currentIndex].children) {
                trigger(currentIndex, true)
            }
            break
        case Qt.Key_Left: backRequested(); break
        case Qt.Key_Escape: backRequested(); break
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Space:
            if (currentIndex >= 0) {
                trigger(currentIndex, true)
            }
            break
        case Qt.Key_Menu:
            closeAllRequested()
            break
        default: {
            // An entry's letter: if only one entry has it, it's run, otherwise
            // focus moves to the next one using it.
            const letter = event.text.toLowerCase()
            if (letter.length !== 1) {
                return
            }
            const matches = []
            entries.forEach((e, i) => { if (selectable(e) && accessKey(e) === letter) matches.push(i) })
            if (matches.length === 0) {
                return
            }
            if (matches.length === 1) {
                currentIndex = matches[0]
                trigger(matches[0], true)
            } else {
                currentIndex = matches.find(i => i > currentIndex) ?? matches[0]
            }
        }
        }
        event.accepted = true
    }

    // --- look ---
    PanelShadow {
        target: panel
        radius: Theme.radiusOverlay
        blur: 16
        offset: Qt.vector2d(0, 4)
        color: Qt.rgba(0, 0, 0, 0.4)
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusOverlay
        color: panel.fill
        border.width: 1
        border.color: Theme.stroke
    }

    // Clicks on the panel don't close the menu.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }

    Column {
        x: 0
        y: panel.padding
        width: panel.width

        Repeater {
            model: panel.entries

            delegate: Item {
                id: row
                required property var modelData
                required property int index
                readonly property var entry: modelData
                readonly property bool isCurrent: panel.currentIndex === index
                readonly property bool usable: entry.enabled !== false
                width: panel.width
                height: panel.rowHeightOf(entry)

                // Separator
                Rectangle {
                    visible: row.entry.separator === true
                    anchors.verticalCenter: parent.verticalCenter
                    x: 0
                    width: parent.width
                    height: 1
                    color: Theme.stroke
                }

                // The icon row of file menus: Cut, Copy, Rename...
                Row {
                    visible: !!row.entry.iconRow
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Repeater {
                        model: row.entry.iconRow || []
                        delegate: Item {
                            id: iconButton
                            required property var modelData
                            readonly property bool usable: modelData.enabled !== false
                            width: 36
                            height: 36
                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.radius
                                color: iconMouse.pressed ? Theme.subtlePressed : Theme.subtleHover
                                opacity: iconMouse.containsMouse && iconButton.usable ? 1 : 0
                            }
                            Image {
                                anchors.centerIn: parent
                                width: 16
                                height: 16
                                opacity: iconButton.usable ? 1 : 0.4
                                source: Theme.icons + encodeURIComponent(iconButton.modelData.icon)
                                sourceSize: Qt.size(width, height)
                            }
                            MouseArea {
                                id: iconMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: iconButton.usable
                                onClicked: panel.activated(iconButton.modelData, -1)
                            }
                        }
                    }
                }

                // Group title
                Text {
                    visible: row.entry.header !== undefined
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.entry.header || ""
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                }

                Item {
                    visible: !row.entry.separator && row.entry.header === undefined && !row.entry.iconRow
                    anchors.fill: parent

                    Rectangle {
                        x: 4
                        width: parent.width - 8
                        height: parent.height
                        radius: Theme.radius
                        color: rowMouse.pressed ? Theme.subtlePressed : Theme.subtleHover
                        opacity: row.isCurrent && row.usable ? 1 : 0
                    }

                    // Icon, or the check mark or dot of choice entries.
                    Item {
                        x: 16
                        width: 16
                        height: 16
                        anchors.verticalCenter: parent.verticalCenter
                        visible: panel.hasIcons
                        opacity: row.usable ? 1 : 0.4
                        Image {
                            anchors.fill: parent
                            visible: row.entry.checked === undefined && !!row.entry.icon
                            source: row.entry.icon
                                ? (row.entry.icon.indexOf(":") > 0 ? row.entry.icon : Theme.icons + encodeURIComponent(row.entry.icon))
                                : ""
                            sourceSize: Qt.size(width, height)
                            smooth: true
                        }
                        Shape {
                            anchors.fill: parent
                            visible: row.entry.checked === true && !row.entry.radio
                            preferredRendererType: Shape.CurveRenderer
                            ShapePath {
                                strokeColor: Theme.text
                                strokeWidth: 1.5
                                fillColor: "transparent"
                                capStyle: ShapePath.RoundCap
                                joinStyle: ShapePath.RoundJoin
                                startX: 3; startY: 8.5
                                PathLine { x: 6.5; y: 12 }
                                PathLine { x: 13; y: 4.5 }
                            }
                        }
                        Rectangle {
                            visible: row.entry.checked === true && row.entry.radio === true
                            anchors.centerIn: parent
                            width: 6
                            height: 6
                            radius: 3
                            color: Theme.text
                        }
                    }

                    Text {
                        x: panel.textLeft
                        width: parent.width - x - 16 - (row.entry.children || row.entry.pin ? 28 : 0) - shortcutText.implicitWidth
                        anchors.verticalCenter: parent.verticalCenter
                        // The keyboard letter shows only when the menu was
                        // opened from the keyboard.
                        textFormat: panel.keyboardMode ? Text.StyledText : Text.PlainText
                        text: panel.keyboardMode ? panel.styledText(row.entry.text) : panel.plainText(row.entry.text)
                        color: row.usable ? Theme.text : Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    Text {
                        id: shortcutText
                        anchors { right: parent.right; rightMargin: row.entry.children ? 36 : 16; verticalCenter: parent.verticalCenter }
                        text: row.entry.shortcut || ""
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                    }
                    // Submenu arrow.
                    Shape {
                        visible: !!row.entry.children
                        anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
                        width: 8
                        height: 10
                        preferredRendererType: Shape.CurveRenderer
                        ShapePath {
                            strokeColor: Theme.textSecondary
                            strokeWidth: 1.2
                            fillColor: "transparent"
                            capStyle: ShapePath.RoundCap
                            joinStyle: ShapePath.RoundJoin
                            startX: 2; startY: 1
                            PathLine { x: 6; y: 5 }
                            PathLine { x: 2; y: 9 }
                        }
                    }
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    enabled: !row.entry.separator && row.entry.header === undefined && !row.entry.iconRow
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onEntered: {
                        panel.currentIndex = row.usable ? row.index : -1
                        panel.hovered(row.index, row.y + panel.padding)
                    }
                    onExited: {
                        if (panel.currentIndex === row.index && !row.entry.children) {
                            panel.currentIndex = -1
                        }
                    }
                    onClicked: mouse => {
                        // Right button: the entry's menu (such as a jump list
                        // file); if it has none, it acts like the left one.
                        if (mouse.button === Qt.RightButton && row.entry.context) {
                            const p = mapToItem(panel.parent, mouse.x, mouse.y)
                            panel.contextRequested(row.entry, p.x, p.y)
                            return
                        }
                        panel.trigger(row.index, false)
                    }
                }

                // The pin (jump list): shown on hover.
                Item {
                    visible: !!row.entry.pin && (rowMouse.containsMouse || pinMouse.containsMouse)
                    anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                    width: 28
                    height: 28
                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.radius
                        color: Theme.subtleHover
                        opacity: pinMouse.containsMouse ? 1 : 0
                    }
                    Shape {
                        anchors.centerIn: parent
                        width: 14
                        height: 14
                        rotation: row.entry.pin && row.entry.pin.pinned ? 0 : 45
                        preferredRendererType: Shape.CurveRenderer
                        ShapePath {
                            strokeColor: Theme.text
                            strokeWidth: 1.2
                            fillColor: row.entry.pin && row.entry.pin.pinned ? Theme.text : "transparent"
                            joinStyle: ShapePath.RoundJoin
                            capStyle: ShapePath.RoundCap
                            startX: 4.5; startY: 1
                            PathLine { x: 9.5; y: 1 }
                            PathLine { x: 9; y: 6 }
                            PathLine { x: 11.5; y: 8.5 }
                            PathLine { x: 2.5; y: 8.5 }
                            PathLine { x: 5; y: 6 }
                            PathLine { x: 4.5; y: 1 }
                        }
                        ShapePath {
                            strokeColor: Theme.text
                            strokeWidth: 1.2
                            capStyle: ShapePath.RoundCap
                            startX: 7; startY: 8.5
                            PathLine { x: 7; y: 13 }
                        }
                    }
                    MouseArea {
                        id: pinMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            row.entry.pin.toggle()
                            panel.pinToggled()
                        }
                    }
                }
            }
        }
    }
}
