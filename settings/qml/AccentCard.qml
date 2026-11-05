// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
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
    // A color's name for screen readers, from its hue and lightness: the
    // palette's colors have none of their own.
    function colorName(hex) {
        const c = Qt.color(hex)
        const hue = c.hslHue * 360
        const base = c.hslSaturation < 0.12 ? qsTr("Gray")
            : hue < 15 || hue >= 345 ? qsTr("Red")
            : hue < 40 ? qsTr("Orange")
            : hue < 70 ? qsTr("Yellow")
            : hue < 165 ? qsTr("Green")
            : hue < 195 ? qsTr("Teal")
            : hue < 255 ? qsTr("Blue")
            : hue < 320 ? qsTr("Purple")
            : qsTr("Pink")
        return c.hslLightness < 0.32 ? qsTr("%1, dark").arg(base)
            : c.hslLightness > 0.62 ? qsTr("%1, light").arg(base)
            : base
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
        // Windows 11's palette as one group of choices: a single stop for Tab,
        // on the color chosen; the arrows move between colors, Space or Enter
        // chooses one. Screen readers hear each color's name and place.
        Grid {
            id: swatches
            objectName: "accentSwatches"
            x: 36
            columns: Math.max(1, Math.floor((parent.width - 36) / 48))
            spacing: 4
            // The color Tab reaches: the last one the arrows went to, else the chosen one.
            property int focusIndex: -1
            readonly property int chosenIndex: card.accents.indexOf(Prefs.accent.toString().toLowerCase())
            readonly property int tabIndex: focusIndex >= 0 ? focusIndex : Math.max(0, chosenIndex)

            function moveFocus(from, step) {
                const to = from + step
                if (to >= 0 && to < card.accents.length) {
                    focusIndex = to
                    swatchRepeater.itemAt(to).forceActiveFocus(Qt.TabFocusReason)
                }
            }

            Repeater {
                id: swatchRepeater
                model: card.accents
                delegate: T.AbstractButton {
                    id: swatch
                    required property string modelData
                    required property int index
                    readonly property bool chosen: Prefs.accent.toString().toLowerCase() === modelData
                    width: 44
                    height: 44
                    hoverEnabled: true
                    focusPolicy: index === swatches.tabIndex ? Qt.TabFocus : Qt.NoFocus
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: card.colorName(modelData)
                    Accessible.description: qsTr("Color %1 of %2").arg(index + 1).arg(card.accents.length)
                    Accessible.checkable: true
                    Accessible.checked: chosen
                    onActiveFocusChanged: if (activeFocus) swatches.focusIndex = index
                    Keys.onLeftPressed: swatches.moveFocus(index, -1)
                    Keys.onRightPressed: swatches.moveFocus(index, 1)
                    Keys.onUpPressed: swatches.moveFocus(index, -swatches.columns)
                    Keys.onDownPressed: swatches.moveFocus(index, swatches.columns)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    onClicked: Prefs.accent = modelData
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }

                    background: Rectangle {
                        radius: Theme.radius
                        color: "transparent"
                        border.width: swatch.chosen ? 2 : swatch.hovered ? 1 : 0
                        border.color: swatch.chosen ? Theme.text : Theme.controlStrokeStrong
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
                        FocusRing {
                            visible: swatch.visualFocus
                        }
                    }
                }
            }
        }
    }
}
