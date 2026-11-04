// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 lowell
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Themes (docs/configuration.md, "Theme files"): a theme
// (theme= in vela.conf) with the colors and corners of the shell and the
// apps, Vela Official (themes/vela.conf) first; below it the surfaces' own
// styles, like Windhawk's three stylers (taskbar-style=, startmenu-style=,
// notifications-style=). Your own go in ~/.config/vela/themes/.
Page {
    id: page
    readonly property var choices: { Prefs.visualTheme; return Prefs.visualThemes() }
    // Browsing themes never mutates vela.conf; Apply is an explicit action.
    property string candidateTheme: Prefs.visualTheme
    property bool customizing: false
    readonly property int foundIndex: choices.findIndex(t => t.name === page.candidateTheme)
    readonly property int chosenIndex: Math.max(0, foundIndex)
    readonly property var chosen: choices[chosenIndex] ?? ({ text: "", author: "" })
    readonly property var candidateTokens: Prefs.themePreview(page.candidateTheme)

    PersonalizationPage.DesktopPreview { themeTokens: page.candidateTokens }

    CardGroup {
        Card {
            icon: "preferences-desktop-theme"
            title: qsTr("Theme")
            description: page.foundIndex < 0
                ? qsTr("\"%1\" isn't installed: Vela's own look is used").arg(page.candidateTheme)
                : page.chosen.author !== ""
                ? qsTr("Original author: %1").arg(page.chosen.author)
                : qsTr("Preview a complete theme before applying it")
            trailing: Choice {
                model: page.choices
                currentIndex: page.chosenIndex
                onChosen: index => page.candidateTheme = page.choices[index].name
            }
        }
        Card {
            icon: "dialog-ok-apply"
            title: page.candidateTheme === Prefs.visualTheme ? qsTr("Theme in use") : qsTr("Apply selected theme")
            description: qsTr("Selecting a theme only changes its preview until you apply it")
            trailing: Button {
                text: qsTr("Apply")
                accent: true
                usable: page.foundIndex >= 0 && page.candidateTheme !== Prefs.visualTheme
                onClicked: Prefs.visualTheme = page.candidateTheme
            }
        }
        Card {
            icon: "configure"
            title: qsTr("Customize")
            description: qsTr("Optional: override individual shell surfaces")
            trailing: Button {
                text: page.customizing ? qsTr("Hide") : qsTr("Customize")
                onClicked: page.customizing = !page.customizing
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
        visible: page.customizing
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
        visible: page.customizing
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
                description: [inUse ? qsTr("In use") : "",
                              modelData.author ? qsTr("Original author: %1").arg(modelData.author) : ""]
                    .filter(s => s !== "").join(" · ")
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
            description: qsTr("Vela Official and no styles. Light or dark mode and the accent stay")
            trailing: Button {
                text: qsTr("Reset")
                usable: Prefs.visualTheme !== "vela" || Object.values(Prefs.surfaceStyles).some(s => s !== "")
                onClicked: { Prefs.resetThemes(); page.candidateTheme = "vela" }
            }
        }
    }
}
