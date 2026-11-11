// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The preview: the wallpaper with a window and the taskbar in miniature, in
// the theme in use or in the tokens given (a theme's card, ThemeGallery's).
// framed: in a frame like a monitor's, as the pages show it on top; the
// cards show the picture alone, 320 x 180 to scale, 16:9 like the pictures
// of the installed themes.
Item {
    id: preview
    property bool framed: true
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
        const roles = light === appsLight ? appsTonal : shellTonal
        if (roles !== null) {
            return themed("primary", light, roles.primary)
        }
        return Theme.accentFillOf(themed("accent", light, Style.accent), light)
    }
    // A theme with the expressive controls draws with Material's tonal
    // roles (Theme.qml), so its miniature does too: the roles of the
    // palette these tokens would start from in a mode (accentsource.h's
    // schemeSource(): the theme's primary, its accent, else the user's, the
    // wallpaper's when asked), or null with the default look.
    function tonalFor(light) {
        if (themed("controls-variant", light, "default") !== "expressive") {
            return null
        }
        const own = themed("primary", light, themed("accent", light, null))
        if (own !== null) {
            return Style.tonalOf(own, light)
        }
        const asks = themed("color-source", light, "accent") === "wallpaper"
        const fromWallpaper = Prefs.accentSource === "wallpaper" || (Prefs.accentSource === "theme" && asks)
        return Style.tonalOf(fromWallpaper && Style.wallpaperColor.valid ? Style.wallpaperColor : Prefs.accent, light)
    }
    readonly property var appsTonal: tonalFor(appsLight)
    readonly property var shellTonal: tonalFor(shellLight)
    width: framed ? parent.width : frame.width
    height: framed ? 210 : frame.height
    Rectangle {
        id: frame
        width: 320
        height: preview.framed ? 196 : 180
        radius: preview.framed ? 8 : 0
        color: preview.framed ? "#101010" : "transparent"
        border.width: preview.framed ? 1 : 0
        border.color: Qt.rgba(1, 1, 1, 0.12)
        Item {
            anchors { fill: parent; margins: preview.framed ? 6 : 0 }
            clip: true
            Image {
                anchors.fill: parent
                source: Prefs.wallpaper.startsWith(":") ? "qrc" + Prefs.wallpaper : "file://" + Prefs.wallpaper
                sourceSize: Qt.size(616, 368)
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
            Rectangle {
                objectName: "previewWindow"
                x: parent.width * 0.22
                y: parent.height * 0.18
                width: parent.width * 0.56
                height: parent.height * 0.52
                radius: 3
                // A window, in the apps' mode.
                // Expressive: Material's surface, as Theme.background.
                color: preview.themed("background", preview.appsLight,
                    preview.appsTonal !== null ? preview.themed("surfaceBase", preview.appsLight, preview.appsTonal.surface)
                    : preview.appsLight ? Qt.rgba(0.95, 0.95, 0.95, 0.97) : Qt.rgba(0.13, 0.13, 0.13, 0.96))
                // The text: the theme's at the opacity of these lines.
                readonly property color line: {
                    const text = preview.themed("text", preview.appsLight,
                        preview.appsTonal !== null ? preview.themed("onSurface", preview.appsLight, preview.appsTonal.onSurface)
                        : preview.appsLight ? Qt.rgba(0, 0, 0, 1) : Qt.rgba(1, 1, 1, 1))
                    return Qt.rgba(text.r, text.g, text.b, 0.2)
                }
                Rectangle { x: 8; y: 8; width: parent.width * 0.4; height: 5; radius: 2; color: preview.accentFill(preview.appsLight) }
                Rectangle { x: 8; y: 20; width: parent.width * 0.7; height: 4; radius: 2; color: parent.line }
                Rectangle { x: 8; y: 30; width: parent.width * 0.55; height: 4; radius: 2; color: parent.line }
            }
            Item {
                id: bar
                // The taskbar in Vela's mode, laid out by the shell's own rule
                // (Style.taskbarLayout(), themefile.h): a bar or a floating
                // pill, or one as wide as its apps (fit) that places them
                // itself, centered or at the left, with the tray in a pill of
                // its own at the edge (edge 1 and 2).
                readonly property var layout: Style.taskbarLayout(preview.themeTokens || {}, preview.shellLight,
                    Prefs.taskbarAlignment)
                // Expressive: surface container, as Theme.taskbar; the apps
                // in on surface variant.
                readonly property color fill: preview.themed("taskbar", preview.shellLight,
                    preview.shellTonal !== null ? preview.themed("surfaceContainer", preview.shellLight, preview.shellTonal.surfaceContainer)
                    : preview.shellLight ? Qt.rgba(0.94, 0.94, 0.95, 0.88) : Qt.rgba(0.11, 0.11, 0.13, 0.85))
                readonly property color dot: {
                    if (preview.shellTonal === null) {
                        return preview.shellLight ? Qt.rgba(0, 0, 0, 0.35) : Qt.rgba(1, 1, 1, 0.45)
                    }
                    const c = preview.shellTonal.onSurfaceVariant
                    return Qt.rgba(c.r, c.g, c.b, 0.6)
                }
                width: parent.width
                height: 14
                y: parent.height - height - (layout.floating ? 4 : 0)

                Rectangle {
                    // The apps' bar, or pill.
                    objectName: "previewBar"
                    width: bar.layout.fit ? icons.width + 16 : bar.layout.floating ? parent.width - 12 : parent.width
                    x: bar.layout.fit && bar.layout.edge === 2 ? 6 : (parent.width - width) / 2
                    height: parent.height
                    radius: bar.layout.floating ? 7 : 0
                    color: bar.fill
                    Row {
                        id: icons
                        x: bar.layout.fit ? 8 : bar.layout.appsAtLeft ? 6 : (parent.width - width) / 2
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Repeater {
                            model: 5
                            Rectangle { width: 8; height: 8; radius: 2; color: index === 0 ? preview.accentFill(preview.shellLight) : bar.dot }
                        }
                    }
                }
                Rectangle {
                    // The tray's own pill at the screen's edge.
                    visible: bar.layout.fit && bar.layout.edge > 0
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    width: 24
                    height: parent.height
                    radius: 7
                    color: bar.fill
                    Row {
                        anchors.centerIn: parent
                        spacing: 3
                        Repeater {
                            model: 2
                            Rectangle { width: 4; height: 4; radius: 2; color: bar.dot }
                        }
                    }
                }
            }
        }
    }
}
