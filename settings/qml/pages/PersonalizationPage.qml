// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization: the desktop preview at the top, then the sections.
Page {
    DesktopPreview {}

    CardGroup {
        LinkCard {
            icon: "preferences-desktop-wallpaper"
            title: qsTr("Background")
            description: qsTr("Desktop background picture")
            onClicked: root.navigate("background")
        }
        LinkCard {
            icon: "preferences-desktop-color"
            title: qsTr("Colors")
            description: qsTr("Accent color, light or dark mode for apps")
            onClicked: root.navigate("colors")
        }
        LinkCard {
            icon: "preferences-desktop-theme"
            title: qsTr("Themes")
            description: qsTr("Colors and corners of the shell and the apps")
            onClicked: root.navigate("themes")
        }
        LinkCard {
            icon: "preferences-system-windows"
            title: qsTr("Taskbar")
            description: qsTr("Alignment, taskbar behaviors")
            onClicked: root.navigate("taskbar")
        }
    }

    // The preview: the wallpaper with a window and the taskbar in miniature.
    component DesktopPreview: Item {
        width: parent.width
        height: 210
        Rectangle {
            id: frame
            width: 320
            height: 196
            radius: 8
            color: "#101010"
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.12)
            Item {
                anchors { fill: parent; margins: 6 }
                clip: true
                Image {
                    anchors.fill: parent
                    source: Prefs.wallpaper.startsWith(":") ? "qrc" + Prefs.wallpaper : "file://" + Prefs.wallpaper
                    sourceSize: Qt.size(616, 368)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                Rectangle {
                    x: parent.width * 0.22
                    y: parent.height * 0.18
                    width: parent.width * 0.56
                    height: parent.height * 0.52
                    radius: 3
                    // A window, in the apps' mode.
                    color: Prefs.light ? Qt.rgba(0.95, 0.95, 0.95, 0.97) : Qt.rgba(0.13, 0.13, 0.13, 0.96)
                    Rectangle { x: 8; y: 8; width: parent.width * 0.4; height: 5; radius: 2; color: Theme.accentFill }
                    Rectangle { x: 8; y: 20; width: parent.width * 0.7; height: 4; radius: 2; color: Prefs.light ? Qt.rgba(0, 0, 0, 0.2) : Qt.rgba(1, 1, 1, 0.2) }
                    Rectangle { x: 8; y: 30; width: parent.width * 0.55; height: 4; radius: 2; color: Prefs.light ? Qt.rgba(0, 0, 0, 0.2) : Qt.rgba(1, 1, 1, 0.2) }
                }
                Rectangle {
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: 14
                    // The taskbar, in Vela's mode.
                    color: Prefs.shellTheme === "light" ? Qt.rgba(0.94, 0.94, 0.95, 0.88) : Qt.rgba(0.11, 0.11, 0.13, 0.85)
                    Row {
                        x: Prefs.taskbarAlignment === "left" ? 6 : (parent.width - width) / 2
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Repeater {
                            model: 5
                            Rectangle { width: 8; height: 8; radius: 2; color: index === 0 ? Theme.accentFill : Prefs.shellTheme === "light" ? Qt.rgba(0, 0, 0, 0.35) : Qt.rgba(1, 1, 1, 0.45) }
                        }
                    }
                }
            }
        }
    }
}
