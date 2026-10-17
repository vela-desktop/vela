// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Dialogs
import Vela.Controls

// "Run" (Win+R, or from the Win+X menu), like in Windows: at the bottom left,
// opens a program, a folder, a document or an address.
Window {
    id: root
    visible: false
    // The dialog with room for its shadow, cut where the window meets the
    // output's edge and the taskbar (docs/visual-coherence.md §6.9): the
    // dialog rests 12 units from both.
    readonly property real room: shadow.reach
    width: 12 + 420 + room
    height: room + 196 + 12
    color: "transparent"

    property bool browsing: false // the "Browse" window has the keyboard: it doesn't close
    property int historyIndex: -1

    function open() {
        field.text = System.runHistory()[0] || ""
        field.selectAll()
        historyIndex = -1
        Menus.placeOnTargetScreen(root)
        visible = true
        motion.open()
        field.forceActiveFocus()
    }
    function close() {
        motion.close()
    }
    function accept() {
        if (System.run(field.text)) {
            close()
        }
    }

    // At the bottom left, above the taskbar: from its coordinates to the
    // output's.
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        return Qt.point(p.x, Screen.height - Theme.taskbarHeight - root.height + p.y)
    }

    Connections {
        target: Shell
        function onRunRequested() {
            root.open()
        }
    }

    // Once the menu closes, the keyboard comes back here; if it went elsewhere
    // instead (a new window, a click), this panel closes too.
    Connections {
        target: Menus
        function onIsOpenChanged() {
            if (!Menus.isOpen && motion.opened) {
                focusCheck.restart()
            }
        }
    }
    Timer {
        id: focusCheck
        interval: 200
        onTriggered: {
            if (!root.active && motion.opened && !Menus.isOpen) {
                root.close()
            }
        }
    }

    onActiveChanged: {
        if (!active && motion.opened && !browsing && !Menus.isOpen) {
            close()
        }
    }

    FileDialog {
        id: browse
        title: qsTr("Browse")
        onAccepted: {
            root.browsing = false
            field.text = selectedFile.toString().replace(/^file:\/\//, "")
            field.forceActiveFocus()
        }
        onRejected: root.browsing = false
    }

    // A click in the shadow's room closes it, like a click outside.
    ShadowRoom {
        panels: [panel]
        onOutside: root.close()
    }

    // It grows in from 1.05, like Windows 11's dialogs, and fades out.
    PanelMotion {
        id: motion
        x: 12
        y: root.room
        width: 420
        height: 196
        scaleFrom: 1.05
        duration: Theme.normal
        onClosed: root.visible = false

        Shadow {
            id: shadow
            target: panel
            depth: Theme.shadow.dialog
        }

        Rectangle {
            id: panel
            anchors.fill: parent
            radius: Theme.radiusOverlay
            color: Theme.dialog
            border.width: 1
            border.color: Theme.stroke

            Icon {
                x: 20
                y: 20
                name: "system-run"
                size: 32
            }
            Text {
                x: 64
                y: 18
                width: parent.width - x - 20
                text: qsTr("Type the name of a program, folder, document or Internet resource, and Vela will open it for you.")
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            Text {
                x: 20
                y: 84
                text: qsTr("Open:")
                color: Theme.text
                font.pixelSize: Theme.fontBody
            }
            Rectangle {
                id: fieldBox
                x: 64
                y: 76
                width: parent.width - x - 20
                height: 32
                radius: Theme.radius
                color: Theme.control
                border.width: 1
                border.color: field.activeFocus ? Theme.accent : Theme.stroke

                MenuTextField {
                    id: field
                    anchors { left: parent.left; right: historyButton.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                    mapToScreen: (x, y) => root.screenPoint(field, x, y)
                    Keys.onEscapePressed: root.close()
                    Keys.onReturnPressed: root.accept()
                    Keys.onEnterPressed: root.accept()
                    // Up and down go through commands already used, like Windows'
                    // list.
                    Keys.onUpPressed: root.step(-1)
                    Keys.onDownPressed: root.step(1)
                }
                // The list of used commands.
                Item {
                    id: historyButton
                    anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
                    width: 28
                    Text {
                        anchors.centerIn: parent
                        text: "⌄"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            const p = root.screenPoint(fieldBox, 0, fieldBox.height + 2)
                            Menus.open(System.runHistory().map(command => ({
                                text: command.replace(/&/g, "&&"),
                                action: () => { field.text = command; field.forceActiveFocus() }
                            })), p.x, p.y, { minWidth: fieldBox.width })
                        }
                    }
                }
            }

            Row {
                anchors { right: parent.right; bottom: parent.bottom; margins: 20 }
                spacing: 8

                Button {
                    text: "OK"
                    accent: true
                    opacity: field.text.trim().length > 0 ? 1 : 0.5
                    onClicked: root.accept()
                }
                Button {
                    text: qsTr("Cancel")
                    onClicked: root.close()
                }
                Button {
                    text: qsTr("Browse...")
                    onClicked: {
                        root.browsing = true
                        browse.open()
                    }
                }
            }
        }
    }

    function step(direction) {
        const history = System.runHistory()
        if (history.length === 0) {
            return
        }
        historyIndex = Math.max(0, Math.min(history.length - 1, historyIndex + direction))
        field.text = history[historyIndex]
    }
}
