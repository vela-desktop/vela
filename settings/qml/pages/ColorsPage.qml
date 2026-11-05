// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Colors: the mode (light, dark, or custom: one for Vela and
// one for apps) and the accent color, with the Windows 11 palette.
Page {
    id: page
    property bool custom: false // "Custom" chosen even with the two modes equal
    // A theme may bring its own accent (accent=, or dark.accent= and
    // light.accent= for one mode), which wins over the one chosen here: the
    // grid says so instead of changing nothing (Theme.qml's themed()).
    readonly property var themeTokens: { Prefs.visualTheme; Prefs.themesRevision; return Prefs.resolveTheme(Prefs.visualTheme).tokens }
    readonly property bool themeAccentDark: "accent" in themeTokens || "dark.accent" in themeTokens
    readonly property bool themeAccentLight: "accent" in themeTokens || "light.accent" in themeTokens
    readonly property bool themeAccent: themeAccentDark && themeAccentLight
    readonly property string themeName: Prefs.visualThemes().find(t => t.name === Prefs.visualTheme)?.text ?? Prefs.visualTheme
    readonly property var accents: [
        "#5b8cff", "#ffb900", "#ff8c00", "#f7630c", "#ca5010", "#da3b01", "#ef6950", "#d13438",
        "#ff4343", "#e74856", "#e81123", "#ea005e", "#c30052", "#e3008c", "#bf0077", "#c239b3",
        "#9a0089", "#0078d4", "#0063b1", "#8e8cd8", "#6b69d6", "#8764b8", "#744da9", "#b146c2",
        "#881798", "#0099bc", "#2d7d9a", "#00b7c3", "#038387", "#00b294", "#018574", "#00cc6a",
        "#10893e", "#7a7574", "#5d5a58", "#68768a", "#515c6b", "#567c73", "#486860", "#498205",
        "#107c10", "#767676", "#4c4a48", "#69797e", "#4a5459", "#647c64", "#525e54", "#847545"
    ]

    DesktopPreview {}

    CardGroup {
        Card {
            icon: "preferences-desktop-theme"
            title: qsTr("Choose your mode")
            description: qsTr("Change the colors of Vela and your apps (light or dark)")
            trailing: Choice {
                model: [qsTr("Light"), qsTr("Dark"), qsTr("Custom")]
                currentIndex: page.custom || Prefs.shellTheme !== Prefs.appTheme ? 2 : Prefs.appTheme === "light" ? 0 : 1
                onChosen: index => {
                    page.custom = index === 2
                    if (index < 2) {
                        const mode = index === 0 ? "light" : "dark"
                        Prefs.shellTheme = mode
                        Prefs.appTheme = mode
                    }
                }
            }
        }
        Card {
            visible: page.custom || Prefs.shellTheme !== Prefs.appTheme
            icon: "preferences-system-windows"
            title: qsTr("Choose your default Vela mode")
            description: qsTr("Taskbar, Start menu, menus and panels")
            trailing: Choice {
                model: [qsTr("Light"), qsTr("Dark")]
                currentIndex: Prefs.shellTheme === "light" ? 0 : 1
                onChosen: index => Prefs.shellTheme = index === 0 ? "light" : "dark"
            }
        }
        Card {
            visible: page.custom || Prefs.shellTheme !== Prefs.appTheme
            icon: "preferences-desktop-default-applications"
            title: qsTr("Choose your default app mode")
            description: qsTr("App windows, File Explorer and Settings")
            trailing: Choice {
                model: [qsTr("Light"), qsTr("Dark")]
                currentIndex: Prefs.appTheme === "light" ? 0 : 1
                onChosen: index => Prefs.appTheme = index === 0 ? "light" : "dark"
            }
        }
        Card {
            icon: "preferences-desktop-color"
            title: qsTr("Accent color")
            description: page.themeAccent
                ? qsTr("%1, the theme in use, brings its own accent color. The one chosen here comes back with another theme").arg(page.themeName)
                : page.themeAccentDark
                ? qsTr("Buttons, selections, active items in the taskbar and menus. In dark mode %1, the theme in use, brings its own").arg(page.themeName)
                : page.themeAccentLight
                ? qsTr("Buttons, selections, active items in the taskbar and menus. In light mode %1, the theme in use, brings its own").arg(page.themeName)
                : qsTr("Buttons, selections, active items in the taskbar and menus")
            trailing: [
                Button {
                    visible: page.themeAccent
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Themes")
                    onClicked: root.navigate("personalization")
                },
                Rectangle {
                    width: 32
                    height: 32
                    radius: Theme.radius
                    // The accent in use: the theme's, when it has one.
                    color: page.themeAccent ? Theme.accent : Prefs.accent
                    border.width: 1
                    border.color: Theme.controlStroke
                }
            ]
            contentItem: Column {
                width: parent.width
                spacing: 8
                enabled: !page.themeAccent
                opacity: enabled ? 1 : 0.45
                Text {
                    text: qsTr("Colors")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    leftPadding: 36
                }
                Grid {
                    x: 36
                    columns: Math.max(1, Math.floor((parent.width - 36) / 48))
                    spacing: 4
                    Repeater {
                        model: page.accents
                        delegate: Rectangle {
                            id: swatch
                            required property string modelData
                            readonly property bool chosen: Prefs.accent.toString().toLowerCase() === modelData
                            width: 44
                            height: 44
                            radius: Theme.radius
                            color: "transparent"
                            border.width: chosen ? 2 : swatchMouse.containsMouse ? 1 : 0
                            border.color: chosen ? Theme.text : Theme.controlStrokeStrong
                            Rectangle {
                                anchors { fill: parent; margins: 4 }
                                radius: 2
                                color: swatch.modelData
                                Text {
                                    visible: swatch.chosen
                                    anchors { right: parent.right; top: parent.top; margins: 2 }
                                    text: "✓"
                                    color: "white"
                                    font.pixelSize: 12
                                    font.weight: Font.Bold
                                }
                            }
                            MouseArea {
                                id: swatchMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Prefs.accent = swatch.modelData
                            }
                        }
                    }
                }
            }
        }
    }
}
