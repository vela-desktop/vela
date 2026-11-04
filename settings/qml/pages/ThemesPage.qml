// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 lowell
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Themes (docs/configuration.md, "Theme files"): a theme
// (theme= in vela.conf) with the colors and corners of the shell and the
// apps; below it the surfaces' own styles, like Windhawk's three stylers
// (taskbar-style=, startmenu-style=, notifications-style=). Your own go in
// ~/.config/vela/themes/.
Page {
    id: page
    readonly property var choices: { Prefs.visualTheme; return [{ name: "", text: qsTr("Vela (default)") }].concat(Prefs.visualThemes()) }
    readonly property int chosenIndex: {
        for (let i = 0; i < choices.length; ++i) {
            if (choices[i].name === Prefs.visualTheme) return i
        }
        return 0 // not installed (any more): Vela's own
    }

    PersonalizationPage.DesktopPreview {}

    CardGroup {
        Card {
            icon: "preferences-desktop-theme"
            title: qsTr("Theme")
            description: Prefs.visualTheme !== "" && page.chosenIndex === 0
                ? qsTr("\"%1\" isn't installed: Vela's own look is used").arg(Prefs.visualTheme)
                : qsTr("Colors and corners of the shell and the apps. A theme can bring its own accent color")
            trailing: Choice {
                model: page.choices
                currentIndex: page.chosenIndex
                onChosen: index => Prefs.visualTheme = page.choices[index].name
            }
        }
    }

    // The surfaces' styles, one surface at a time, with their pictures.
    property string surface: "taskbar"
    readonly property var surfaces: [
        { surface: "taskbar", text: qsTr("Taskbar") },
        { surface: "startmenu", text: qsTr("Start menu") },
        { surface: "notifications", text: qsTr("Quick settings") }
    ]
    // Read again when the surface changes or a style is chosen.
    readonly property var gallery: { Prefs.surfaceStyles; return Prefs.styleGallery(surface) }
    readonly property string chosenStyle: Prefs.surfaceStyles[surface] || ""

    Row {
        spacing: 8
        Repeater {
            model: page.surfaces
            Button {
                required property var modelData
                text: modelData.text
                accent: page.surface === modelData.surface
                onClicked: page.surface = modelData.surface
            }
        }
    }

    CardGroup {
        title: qsTr("Styles")
        Card {
            title: qsTr("None")
            description: qsTr("As the theme draws it. Your own styles: ~/.config/vela/themes/%1/<name>.conf").arg(page.surface)
            trailing: Button {
                text: page.chosenStyle === "" ? qsTr("In use") : qsTr("Use")
                accent: page.chosenStyle !== ""
                usable: page.chosenStyle !== ""
                onClicked: Prefs.setSurfaceStyle(page.surface, "")
            }
        }
        Repeater {
            model: page.gallery
            Card {
                id: styleCard
                required property var modelData
                readonly property bool inUse: page.chosenStyle === modelData.name
                title: modelData.text
                description: inUse ? qsTr("In use") : ""
                trailing: Button {
                    text: styleCard.inUse ? qsTr("In use") : qsTr("Use")
                    accent: !styleCard.inUse
                    usable: !styleCard.inUse
                    onClicked: Prefs.setSurfaceStyle(page.surface, styleCard.modelData.name)
                }
                // The pictures side by side (a taskbar's is wide, quick
                // settings' two are tall); none for a style without.
                contentItem: Row {
                    visible: count > 0
                    width: parent ? parent.width : 0
                    spacing: 12
                    readonly property int count: styleCard.modelData.images.length
                    readonly property real each: (width - spacing * (Math.max(count, 1) - 1)) / Math.max(count, 1)
                    Repeater {
                        model: styleCard.modelData.images
                        Image {
                            required property string modelData
                            width: parent.each
                            height: implicitWidth > 0 ? Math.min(width * implicitHeight / implicitWidth, 340) : 0
                            fillMode: Image.PreserveAspectFit
                            source: modelData
                            asynchronous: true
                            smooth: true
                        }
                    }
                }
            }
        }
    }

    CardGroup {
        Card {
            icon: "edit-undo"
            title: qsTr("Reset")
            description: qsTr("Vela's own look: no theme and no styles. Light or dark mode and the accent stay")
            trailing: Button {
                text: qsTr("Reset")
                usable: Prefs.visualTheme !== "" || Object.values(Prefs.surfaceStyles).some(s => s !== "")
                onClicked: Prefs.resetThemes()
            }
        }
    }
}
