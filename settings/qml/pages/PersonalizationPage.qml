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

    // The preview: the wallpaper with a window and the taskbar in miniature,
    // in the theme in use, or in the one Personalization > Themes previews.
    component DesktopPreview: Item {
        id: preview
        // A theme's tokens (themefile.h): the one in use, as Theme.qml takes it.
        property var themeTokens: Style.theme
        readonly property bool appsLight: Prefs.light
        readonly property bool shellLight: Prefs.shellTheme === "light"
        // Theme.qml's themed(), for a given mode: the window is in the apps'
        // mode, the taskbar in Vela's.
        function themed(key, light, fallback) {
            const t = themeTokens || {}
            const modeKey = (light ? "light." : "dark.") + key
            return modeKey in t ? t[modeKey] : (key in t ? t[key] : fallback)
        }
        function accentFill(light) {
            return Theme.accentFillOf(themed("accent", light, Style.accent), light)
        }
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
                    color: preview.themed("background", preview.appsLight,
                        preview.appsLight ? Qt.rgba(0.95, 0.95, 0.95, 0.97) : Qt.rgba(0.13, 0.13, 0.13, 0.96))
                    // The text: the theme's at the opacity of these lines.
                    readonly property color line: {
                        const text = preview.themed("text", preview.appsLight, preview.appsLight ? Qt.rgba(0, 0, 0, 1) : Qt.rgba(1, 1, 1, 1))
                        return Qt.rgba(text.r, text.g, text.b, 0.2)
                    }
                    Rectangle { x: 8; y: 8; width: parent.width * 0.4; height: 5; radius: 2; color: preview.accentFill(preview.appsLight) }
                    Rectangle { x: 8; y: 20; width: parent.width * 0.7; height: 4; radius: 2; color: parent.line }
                    Rectangle { x: 8; y: 30; width: parent.width * 0.55; height: 4; radius: 2; color: parent.line }
                }
                Rectangle {
                    // The taskbar, in Vela's mode, with its style's shape
                    // (Styles.qml): a floating pill, or one as wide as the
                    // apps on it (fit), as a style or a variant draws it.
                    readonly property string variant: preview.themed("taskbar-variant", preview.shellLight, "default")
                    readonly property bool floating: variant !== "default"
                        || preview.themed("taskbar-margin", preview.shellLight, 0) > 0
                        || preview.themed("taskbar-radius", preview.shellLight, 0) > 0
                    readonly property bool fit: preview.themed("taskbar-fit", preview.shellLight, variant !== "default" ? 1 : 0) !== 0
                    width: fit ? icons.width + 16 : floating ? parent.width - 12 : parent.width
                    x: (parent.width - width) / 2
                    y: parent.height - height - (floating ? 4 : 0)
                    height: 14
                    radius: floating ? 7 : 0
                    color: preview.themed("taskbar", preview.shellLight,
                        preview.shellLight ? Qt.rgba(0.94, 0.94, 0.95, 0.88) : Qt.rgba(0.11, 0.11, 0.13, 0.85))
                    Row {
                        id: icons
                        x: Prefs.taskbarAlignment === "left" && !parent.fit ? 6 : (parent.width - width) / 2
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Repeater {
                            model: 5
                            Rectangle { width: 8; height: 8; radius: 2; color: index === 0 ? preview.accentFill(preview.shellLight) : preview.shellLight ? Qt.rgba(0, 0, 0, 0.35) : Qt.rgba(1, 1, 1, 0.45) }
                        }
                    }
                }
            }
        }
    }
}
