// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// What to share, when an app (video call, recording) asks for the screen
// through the portal: a whole output or a window, with previews. The answer
// goes back to xdg-desktop-portal-wlr (see "vela-shell --choose-source" and
// session/xdpw-vela.conf.in).
Window {
    id: root
    visible: false
    // The dialog with room for its shadow around it (docs/visual-coherence.md
    // §6.9).
    readonly property real room: shadow.reach
    width: room + 760 + room
    height: room + 540 + room
    color: "transparent"

    property int tab: 0 // 0 outputs, 1 windows
    property string selected: "" // the answer for the portal
    readonly property var current: tab === 0 ? screens : windows

    ListModel { id: screens }
    ListModel { id: windows }

    Connections {
        target: Shell
        function onChooseSourceRequested() {
            screens.clear()
            for (const screen of Qt.application.screens) {
                if (screen.name !== "") {
                    screens.append({
                        identifier: "screen:" + screen.name,
                        answer: "Monitor: " + screen.name,
                        // The monitor's model: the resolution isn't known here
                        // (at fractional scales devicePixelRatio is the
                        // integer one).
                        title: screen.model !== "" ? screen.name + " (" + screen.model + ")" : screen.name,
                        icon: "video-display",
                        revision: 0
                    })
                }
            }
            windows.clear()
            const ids = []
            for (const w of Capture.windowList()) {
                windows.append({
                    identifier: w.id,
                    answer: "Window: " + w.id,
                    title: w.title !== "" ? w.title : w.appId,
                    icon: Apps.iconForAppId(w.appId),
                    revision: 0
                })
                ids.push(w.id)
            }
            // First the photos of the outputs, then the window: otherwise it
            // would be in the previews itself.
            Capture.captureScreens()
            Capture.capture(ids)
            root.tab = 0
            root.selected = screens.count === 1 ? screens.get(0).answer : ""
            showTimer.restart()
        }
        function onChooseSourceCancelled() {
            showTimer.stop()
            motion.close()
        }
    }

    Timer {
        id: showTimer
        interval: 300 // at most: if a capture doesn't arrive, it opens anyway
        onTriggered: {
            root.visible = true
            motion.open()
            panel.forceActiveFocus()
        }
    }

    Connections {
        target: Capture
        function onThumbnailReady(identifier) {
            for (const model of [screens, windows]) {
                for (let i = 0; i < model.count; ++i) {
                    if (model.get(i).identifier === identifier) {
                        model.setProperty(i, "revision", model.get(i).revision + 1)
                    }
                }
            }
            // All outputs photographed: it can open.
            if (showTimer.running) {
                let ready = true
                for (let i = 0; i < screens.count; ++i) {
                    ready = ready && screens.get(i).revision > 0
                }
                if (ready) {
                    showTimer.stop()
                    showTimer.triggered()
                }
            }
        }
    }

    function answer(text) {
        motion.close()
        Shell.chooseSource(text)
    }

    // A click in the shadow's room is a click outside: the dialog stays,
    // the app waits for an answer.
    ShadowRoom {
        panels: [panel]
    }

    // It grows in from 1.05, like Windows 11's dialogs, and fades out.
    PanelMotion {
        id: motion
        x: root.room
        y: root.room
        width: 760
        height: 540
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
            focus: true
            Keys.onEscapePressed: root.answer("")
            Keys.onReturnPressed: if (root.selected !== "") root.answer(root.selected)
            Keys.onEnterPressed: if (root.selected !== "") root.answer(root.selected)
            Keys.onTabPressed: root.tab = 1 - root.tab

            Text {
                x: 24
                y: 20
                text: qsTr("Choose what to share")
                color: Theme.text
                font.pixelSize: Theme.fontBody + 4
                font.weight: Font.DemiBold
            }
            Text {
                x: 24
                y: 50
                text: qsTr("An app wants to see your screen. It will only see what you choose here.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }

            // The two tabs, underlined like in Windows 11.
            Row {
                x: 24
                y: 84
                spacing: 4
                Repeater {
                    model: [qsTr("Full screen"), qsTr("Window")]
                    delegate: Item {
                        required property string modelData
                        required property int index
                        width: label.implicitWidth + 24
                        height: 36
                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radius
                            color: Theme.subtleHover
                            opacity: tabMouse.containsMouse ? 1 : 0
                        }
                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: parent.modelData
                            color: root.tab === parent.index ? Theme.text : Theme.textSecondary
                            font.pixelSize: Theme.fontBody
                            font.weight: root.tab === parent.index ? Font.DemiBold : Font.Normal
                        }
                        Rectangle {
                            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom }
                            width: 20
                            height: 3
                            radius: 1.5
                            color: Theme.accent
                            visible: root.tab === parent.index
                        }
                        MouseArea {
                            id: tabMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: root.tab = parent.index
                        }
                    }
                }
            }

            GridView {
                id: grid
                x: 16
                y: 132
                width: parent.width - 32
                height: parent.height - y - 76
                clip: true
                model: root.current
                cellWidth: Math.floor(width / 3)
                cellHeight: 186
                boundsBehavior: Flickable.StopAtBounds

                delegate: Item {
                    id: card
                    required property string identifier
                    required property string answer
                    required property string title
                    required property string icon
                    required property int revision
                    readonly property bool chosen: root.selected === answer
                    width: grid.cellWidth
                    height: grid.cellHeight

                    Rectangle {
                        anchors { fill: parent; margins: 6 }
                        radius: Theme.radiusOverlay
                        color: card.chosen ? Qt.rgba(0.36, 0.55, 1.0, 0.18) : cardMouse.containsMouse ? Theme.subtleHover : "transparent"
                        border.width: card.chosen ? 2 : 0
                        border.color: Theme.accent
                    }

                    Rectangle {
                        id: frame
                        x: 16
                        y: 14
                        width: parent.width - 32
                        height: 124
                        radius: 4
                        color: "#101014"
                        clip: true
                        Image {
                            id: thumbnail
                            anchors.fill: parent
                            fillMode: Image.PreserveAspectFit
                            source: card.revision > 0 ? "image://thumbnail/" + card.identifier + "/" + card.revision : ""
                            smooth: true
                            mipmap: true
                        }
                        Image {
                            anchors.centerIn: parent
                            visible: thumbnail.status !== Image.Ready
                            width: 48
                            height: 48
                            source: Theme.icons + encodeURIComponent(card.icon)
                            sourceSize: Qt.size(width, height)
                        }
                    }
                    Row {
                        x: 16
                        anchors { top: frame.bottom; topMargin: 10 }
                        width: parent.width - 32
                        spacing: 8
                        Image {
                            width: 16
                            height: 16
                            source: Theme.icons + encodeURIComponent(card.icon)
                            sourceSize: Qt.size(width, height)
                        }
                        Text {
                            width: parent.width - 24
                            text: card.title
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        id: cardMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.selected = card.answer
                        onDoubleClicked: root.answer(card.answer)
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: grid.count === 0
                    text: root.tab === 0 ? qsTr("No screens") : qsTr("No open windows")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }
            }

            Row {
                anchors { right: parent.right; bottom: parent.bottom; margins: 20 }
                spacing: 8

                Button {
                    text: qsTr("Share")
                    accent: true
                    usable: root.selected !== ""
                    onClicked: root.answer(root.selected)
                }
                Button {
                    text: qsTr("Cancel")
                    onClicked: root.answer("")
                }
            }
        }
    }
}
