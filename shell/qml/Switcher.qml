// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Alt+Tab: the open windows with previews, most recent first. The compositor
// handles the keyboard and tells us what to show and what's selected: here we
// just draw.
Window {
    id: root

    visible: false
    width: Screen.width
    height: Screen.height
    color: "transparent"

    property int selected: 0

    readonly property int cardWidth: 272
    readonly property int cardHeight: 212
    readonly property int spacing: 8
    readonly property int perRow: Math.max(1, Math.min(windows.count,
        Math.floor((width - 96 + spacing) / (cardWidth + spacing))))

    ListModel { id: windows }

    // Like on Windows: a quick Alt+Tab switches window without flashing the
    // panel.
    Timer {
        id: showDelay
        interval: 100
        onTriggered: {
            // Alt+Tab has no taskbar origin: always use the primary output.
            Shell.placeOnScreen(root, Shell.primaryScreen)
            root.visible = true
            motion.open()
            Effects.setBlur(root, [motion.visibleArea])
        }
    }

    Connections {
        target: Shell
        function onSwitcherShown(selected, identifiers) {
            windows.clear()
            for (const id of identifiers) {
                windows.append({
                    identifier: id,
                    title: Capture.title(id),
                    icon: Apps.iconForAppId(Capture.appId(id)),
                    revision: 0
                })
            }
            root.selected = selected
            Capture.capture(identifiers)
            showDelay.restart()
        }
        function onSwitcherSelected(selected) {
            root.selected = selected
        }
        function onSwitcherHidden() {
            showDelay.stop()
            motion.close()
        }
    }

    Connections {
        target: Capture
        function onThumbnailReady(identifier) {
            for (let i = 0; i < windows.count; ++i) {
                if (windows.get(i).identifier === identifier) {
                    windows.setProperty(i, "revision", windows.get(i).revision + 1)
                }
            }
        }
    }

    // It grows in from 0.96, and goes at once (docs/visual-coherence.md K5).
    // The blurred background only under the panel (the window covers the
    // output).
    PanelMotion {
        id: motion
        anchors.centerIn: parent
        width: flow.width + 32
        height: flow.height + 32
        scaleFrom: 0.96
        duration: Theme.normal
        instantExit: true
        onClosed: root.visible = false
        onVisibleAreaChanged: Effects.setBlur(root, [visibleArea])

        Shadow {
            target: panel
        }

        Rectangle {
            id: panel
            anchors.fill: parent
            radius: Theme.radiusOverlay
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke

            Flow {
                id: flow
                anchors.centerIn: parent
                width: root.perRow * root.cardWidth + (root.perRow - 1) * root.spacing
                spacing: root.spacing

                Repeater {
                    model: windows

                    Fill {
                        id: card
                        required property int index
                        required property string identifier
                        required property string title
                        required property string icon
                        required property int revision
                        readonly property bool current: index === root.selected

                        width: root.cardWidth
                        height: root.cardHeight
                        radius: Theme.radiusOverlay
                        color: current || cardMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                        border.width: current ? 2 : 0
                        border.color: Theme.accent

                        // A click on a preview takes us there at once.
                        MouseArea {
                            id: cardMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: Shell.pickSwitcher(card.index)
                        }

                        // Icon and title above the preview.
                        Row {
                            id: header
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
                            spacing: 8

                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                name: card.icon
                            }
                            Text {
                                width: header.width - 24
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.title
                                color: Theme.text
                                font.pixelSize: Theme.fontCaption
                                elide: Text.ElideRight
                            }
                        }

                        Item {
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; top: header.bottom; margins: 12 }

                            Image {
                                id: thumbnail
                                anchors.fill: parent
                                fillMode: Image.PreserveAspectFit
                                source: card.revision > 0 ? "image://thumbnail/" + card.identifier + "/" + card.revision : ""
                                cache: false
                                smooth: true
                            }
                            // Until the preview exists: the big icon.
                            Icon {
                                anchors.centerIn: parent
                                visible: thumbnail.status !== Image.Ready
                                name: card.icon
                                size: 48
                            }
                        }
                    }
                }
            }
        }
    }
}
