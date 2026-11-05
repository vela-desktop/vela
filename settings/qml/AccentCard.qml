// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Colors' accent color: Windows 11's palette, and what the theme in use does
// to it. A theme may bring its own accent, for both modes (accent=) or for
// one (dark.accent=, light.accent=), and it wins in that mode (Theme.qml's
// themed()). Nothing is silently ignored: when every mode in use (Vela's and
// the apps') takes the theme's, the grid is disabled and says why; when only
// one does, the card says in which mode the theme's shows. The swatch is the
// accent the apps use now.
Card {
    id: card
    readonly property var themeTokens: { Prefs.visualTheme; Prefs.themesRevision; return Prefs.resolveTheme(Prefs.visualTheme).tokens }
    readonly property bool themeDark: "accent" in themeTokens || "dark.accent" in themeTokens
    readonly property bool themeLight: "accent" in themeTokens || "light.accent" in themeTokens
    readonly property bool appsLight: Prefs.light
    readonly property bool shellLight: Prefs.shellTheme === "light"
    // The one chosen here shows nowhere now: Vela and the apps both take the theme's.
    readonly property bool overridden: owns(appsLight) && owns(shellLight)
    readonly property string themeName: Prefs.visualThemes().find(t => t.name === Prefs.visualTheme)?.text ?? Prefs.visualTheme
    // The accent the apps use now: the theme's in their mode, else the one chosen.
    readonly property color inUse: owns(appsLight) ? themeAccent(appsLight) : Prefs.accent
    readonly property var accents: [
        "#5b8cff", "#ffb900", "#ff8c00", "#f7630c", "#ca5010", "#da3b01", "#ef6950", "#d13438",
        "#ff4343", "#e74856", "#e81123", "#ea005e", "#c30052", "#e3008c", "#bf0077", "#c239b3",
        "#9a0089", "#0078d4", "#0063b1", "#8e8cd8", "#6b69d6", "#8764b8", "#744da9", "#b146c2",
        "#881798", "#0099bc", "#2d7d9a", "#00b7c3", "#038387", "#00b294", "#018574", "#00cc6a",
        "#10893e", "#7a7574", "#5d5a58", "#68768a", "#515c6b", "#567c73", "#486860", "#498205",
        "#107c10", "#767676", "#4c4a48", "#69797e", "#4a5459", "#647c64", "#525e54", "#847545"
    ]

    function owns(light) {
        return light ? themeLight : themeDark
    }
    function themeAccent(light) {
        const modeKey = (light ? "light." : "dark.") + "accent"
        return modeKey in themeTokens ? themeTokens[modeKey] : themeTokens["accent"]
    }

    icon: "preferences-desktop-color"
    title: qsTr("Accent color")
    description: themeDark && themeLight
        ? qsTr("%1, the theme in use, brings its own accent color. The one chosen here comes back with another theme").arg(themeName)
        : overridden && themeDark
        ? qsTr("In dark mode %1, the theme in use, brings its own accent color. The one chosen here shows in light mode").arg(themeName)
        : overridden
        ? qsTr("In light mode %1, the theme in use, brings its own accent color. The one chosen here shows in dark mode").arg(themeName)
        : themeDark
        ? qsTr("Buttons, selections, active items in the taskbar and menus. In dark mode %1, the theme in use, brings its own").arg(themeName)
        : themeLight
        ? qsTr("Buttons, selections, active items in the taskbar and menus. In light mode %1, the theme in use, brings its own").arg(themeName)
        : qsTr("Buttons, selections, active items in the taskbar and menus")
    trailing: [
        Button {
            visible: card.overridden
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Themes")
            onClicked: root.navigate("themes")
        },
        Rectangle {
            objectName: "accentInUse"
            width: 32
            height: 32
            radius: Theme.radius
            color: card.inUse
            border.width: 1
            border.color: Theme.controlStroke
        }
    ]
    contentItem: Column {
        objectName: "accentGrid"
        width: parent.width
        spacing: 8
        enabled: !card.overridden
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
                model: card.accents
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
