// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
import Vela.Controls

// Colors' accent color: where it comes from, Windows 11's palette, and what
// the theme in use does to it. A theme may bring its own accent, for both
// modes (accent=) or for one (dark.accent=, light.accent=), and it wins in
// that mode (Theme.qml's themed()). Else the source chosen at the top decides
// (appearance/accentSource, shell/src/accentsource.h): the theme's wish
// (color-source=wallpaper takes the wallpaper's color), the wallpaper's
// color, or the one chosen here; a wallpaper without a usable color gives
// the one chosen here. Nothing is silently ignored: the palette is enabled
// only while the color chosen in it shows in a mode in use (Vela's or the
// apps'), and the card always says why it isn't, or in which mode the
// theme's shows. The swatch is the accent the apps use now. Under the
// palette, any other color: the spectrum or its hex code (AccentPicker.qml).
Card {
    id: card
    readonly property var themeTokens: { Prefs.visualTheme; Prefs.themesRevision; return Prefs.resolveTheme(Prefs.visualTheme).tokens }
    readonly property bool themeDark: "accent" in themeTokens || "dark.accent" in themeTokens
    readonly property bool themeLight: "accent" in themeTokens || "light.accent" in themeTokens
    readonly property bool appsLight: Prefs.light
    readonly property bool shellLight: Prefs.shellTheme === "light"
    // The one chosen here shows nowhere now: Vela and the apps both take the theme's.
    readonly property bool overridden: owns(appsLight) && owns(shellLight)
    // The wallpaper's color ("" if it has none usable), next to the choice.
    property string wallpaperColor: Style.wallpaperColor.valid ? Style.wallpaperColor.toString() : ""
    readonly property var sources: ["theme", "wallpaper", "chosen"]
    // The mode the user's accent shows in: the apps', unless the theme's
    // accent has it.
    readonly property bool userLight: owns(appsLight) ? shellLight : appsLight
    // Where the user's accent comes from in that mode: "wallpaper", "chosen",
    // or "fallback" (the wallpaper asked for, without a usable color).
    readonly property string userOrigin: origin(userLight)
    // The one chosen here shows in a mode in use: what enables the palette.
    readonly property bool chosenInUse: shows(appsLight) || shows(shellLight)
    readonly property string themeName: { Prefs.themesRevision; return Prefs.displayName("", Prefs.visualTheme) || Prefs.visualTheme }
    // The accent the apps use now: the theme's in their mode, else the one chosen.
    readonly property color inUse: owns(appsLight) ? themeAccent(appsLight)
        : origin(appsLight) === "wallpaper" ? wallpaperColor : Prefs.accent
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
    // The theme asks for the wallpaper's color in a mode (color-source).
    function asksWallpaper(light) {
        const modeKey = (light ? "light." : "dark.") + "color-source"
        return (modeKey in themeTokens ? themeTokens[modeKey] : themeTokens["color-source"]) === "wallpaper"
    }
    // accentsource.h's userAccent(), for the card's words.
    function origin(light) {
        const fromWallpaper = Prefs.accentSource === "wallpaper"
            || (Prefs.accentSource === "theme" && asksWallpaper(light))
        return !fromWallpaper ? "chosen" : wallpaperColor !== "" ? "wallpaper" : "fallback"
    }
    function shows(light) {
        return !owns(light) && origin(light) !== "wallpaper"
    }
    function themeAccent(light) {
        const modeKey = (light ? "light." : "dark.") + "accent"
        return modeKey in themeTokens ? themeTokens[modeKey] : themeTokens["accent"]
    }

    icon: "preferences-desktop-color"
    title: qsTr("Accent color")
    // Each state says why: the theme's accent, the wallpaper's (the user's
    // choice or the theme's), the one chosen here standing in for a
    // wallpaper without color; and in which mode a theme's own shows.
    readonly property string modeNote: themeDark
        ? qsTr("In dark mode %1, the theme in use, brings its own").arg(themeName)
        : themeLight ? qsTr("In light mode %1, the theme in use, brings its own").arg(themeName) : ""
    readonly property string sourceText: userOrigin === "wallpaper" && Prefs.accentSource === "wallpaper"
        ? qsTr("Taken from your wallpaper: it changes with it. Choose My color to pick one below")
        : userOrigin === "wallpaper"
        ? qsTr("%1, the theme in use, takes it from your wallpaper: it changes with it. Choose My color to pick one below").arg(themeName)
        : userOrigin === "fallback" && Prefs.accentSource === "wallpaper"
        ? qsTr("Your wallpaper has no color to take, so the one chosen below is used")
        : userOrigin === "fallback"
        ? qsTr("%1, the theme in use, takes it from your wallpaper, which has no color to take, so the one chosen below is used").arg(themeName)
        : ""
    description: themeDark && themeLight
        ? qsTr("%1, the theme in use, brings its own accent color. The one chosen here comes back with another theme").arg(themeName)
        : overridden && themeDark
        ? qsTr("In dark mode %1, the theme in use, brings its own accent color. The one chosen here shows in light mode").arg(themeName)
        : overridden
        ? qsTr("In light mode %1, the theme in use, brings its own accent color. The one chosen here shows in dark mode").arg(themeName)
        : sourceText !== ""
        ? (modeNote !== "" ? sourceText + ". " + modeNote : sourceText)
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
        width: parent.width
        spacing: 8
        // Where the accent comes from, and the wallpaper's color it would take.
        Item {
            x: 36
            width: parent.width - 36
            height: 40
            Text {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                text: qsTr("Source")
                color: Theme.text
                font.pixelSize: Theme.fontBody
            }
            Row {
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                spacing: 12
                Rectangle {
                    objectName: "accentWallpaperColor"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: card.wallpaperColor !== ""
                    width: 24
                    height: 24
                    radius: Theme.expressive ? height / 2 : Theme.radius
                    color: card.wallpaperColor !== "" ? card.wallpaperColor : "transparent"
                    border.width: 1
                    border.color: Theme.controlStroke
                    Accessible.role: Accessible.Graphic
                    Accessible.name: visible ? qsTr("Wallpaper color: %1").arg(card.colorName(card.wallpaperColor)) : ""
                }
                Text {
                    objectName: "accentNoWallpaperColor"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: card.wallpaperColor === ""
                    text: qsTr("No color in the wallpaper")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                }
                Choice {
                    objectName: "accentSource"
                    anchors.verticalCenter: parent.verticalCenter
                    model: [qsTr("From the theme"), qsTr("From the wallpaper"), qsTr("My color")]
                    currentIndex: Math.max(0, card.sources.indexOf(Prefs.accentSource))
                    // The theme's accent wins in every mode in use: the
                    // description says so, and the source changes nothing.
                    usable: !card.overridden
                    Accessible.name: qsTr("Accent color source")
                    onChosen: index => Prefs.accentSource = card.sources[index]
                }
            }
        }
        Column {
            objectName: "accentGrid"
            width: parent.width
            spacing: 8
            enabled: card.chosenInUse
            opacity: enabled ? 1 : 0.45
            Text {
                text: qsTr("Colors")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                leftPadding: 36
            }
            // Windows 11's palette as one group of choices: Tab stops on the color
            // chosen (the first if it is none of these), the arrows move between
            // colors, Space or Enter chooses one. Screen readers hear each
            // color's name and place.
            Grid {
                id: swatches
                objectName: "accentSwatches"
                x: 36
                columns: Math.max(1, Math.floor((parent.width - 36) / 48))
                spacing: 4
                readonly property bool noneChosen: card.accents.indexOf(Prefs.accent.toString().toLowerCase()) < 0

                function moveFocus(from, step) {
                    const to = from + step
                    if (to >= 0 && to < card.accents.length) {
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
                        // While it has the focus too, as PictureTile says.
                        focusPolicy: chosen || activeFocus || (index === 0 && swatches.noneChosen) ? Qt.TabFocus : Qt.NoFocus
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: card.colorName(modelData)
                        Accessible.description: qsTr("Color %1 of %2").arg(index + 1).arg(card.accents.length)
                        Accessible.checkable: true
                        Accessible.checked: chosen
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
            // A color of your own: chosen, it shows here with its code.
            Item {
                x: 36
                width: parent.width - 36
                height: 40
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Rectangle {
                        objectName: "accentCustom"
                        visible: swatches.noneChosen
                        anchors.verticalCenter: parent.verticalCenter
                        width: 36
                        height: 36
                        radius: Theme.radius
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.text
                        Rectangle {
                            anchors { fill: parent; margins: 4 }
                            radius: 2
                            color: Prefs.accent
                        }
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            text: qsTr("Custom color")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                        }
                        Text {
                            visible: swatches.noneChosen
                            text: Prefs.accent.toString().toUpperCase()
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                }
                Button {
                    objectName: "accentViewColors"
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    text: qsTr("View colors")
                    onClicked: picker.start(Prefs.accent)
                }
            }
        }
    }

    // Over the whole window, as Settings' other dialogs.
    AccentPicker {
        id: picker
        objectName: "accentPicker"
        parent: root.contentItem !== undefined ? root.contentItem : root
        onAccepted: color => Prefs.accent = color
    }
}
