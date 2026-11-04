// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 lowell
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Themes: a theme file (theme= in vela.conf), a complete
// look of the shell and the apps (docs/configuration.md, "Theme files").
// Choosing a theme only shows it in the preview; Apply makes it the one in
// use, Cancel goes back to it. Below it, Customize: each surface's own style
// over the theme, like Windhawk's three stylers (taskbar-style=,
// startmenu-style=, notifications-style=). Your own themes and styles go in
// ~/.config/vela/themes/.
Page {
    id: page
    readonly property var choices: { Prefs.visualTheme; Prefs.themesRevision; return Prefs.visualThemes() }
    // The theme in the preview: the one in use until another is chosen.
    property string previewed: Prefs.visualTheme
    readonly property bool inUse: previewed === Prefs.visualTheme
    readonly property int previewedIndex: choices.findIndex(t => t.name === previewed)
    readonly property var entry: previewedIndex >= 0 ? choices[previewedIndex] : ({ name: previewed, text: previewed, author: "" })
    // As the shell would take it (themefile.h): its tokens, what is wrong in
    // it; again when a theme file changes (themesRevision).
    readonly property var resolved: { Prefs.visualTheme; Prefs.themesRevision; return Prefs.resolveTheme(previewed) }
    readonly property bool ownAccent: ["accent", "dark.accent", "light.accent"].some(key => key in resolved.tokens)

    Connections {
        target: Prefs
        // Applied, reset, or changed in vela.conf: the preview follows.
        function onVisualThemeChanged() { page.previewed = Prefs.visualTheme }
    }

    function nameOf(theme) {
        const index = choices.findIndex(t => t.name === theme)
        return index >= 0 ? choices[index].text : theme
    }

    // A problem of themefile.h (Problem, toVariant()) in a line of the page.
    function describe(problem) {
        let what = ""
        switch (problem.kind) {
        case "not-key-value": what = qsTr("not a key=value line"); break
        case "unknown-token": what = qsTr("%1 is not a theme token").arg(problem.key); break
        case "expects-color": what = qsTr("%1 needs a color like #RRGGBB, not \"%2\"").arg(problem.key).arg(problem.value); break
        case "expects-number": what = qsTr("%1 needs a number, not \"%2\"").arg(problem.key).arg(problem.value); break
        case "unknown-variant": what = qsTr("%1=%2 isn't one of Vela's layouts").arg(problem.key).arg(problem.value); break
        case "other-surface": what = qsTr("%1 belongs to another surface than this style's").arg(problem.key); break
        case "invalid-parent": what = qsTr("inherits=%1 is not a theme name").arg(problem.value); break
        case "missing-parent": what = qsTr("inherits=%1, which isn't installed").arg(problem.value); break
        case "inheritance-cycle": what = qsTr("inherits=%1, which inherits back from this theme").arg(problem.value); break
        case "chain-too-long": what = qsTr("inherits=%1 makes too long a chain of themes").arg(problem.value); break
        case "unsupported-schema": what = qsTr("schema-version=%1 isn't a format this Vela reads: the file isn't used").arg(problem.value); break
        default: what = problem.kind
        }
        const file = problem.file.substring(problem.file.lastIndexOf("/") + 1)
        return (problem.line > 0 ? file + ":" + problem.line : file) + ": " + what
    }

    PersonalizationPage.DesktopPreview { themeTokens: page.resolved.tokens }

    CardGroup {
        Card {
            icon: "preferences-desktop-theme"
            title: qsTr("Theme")
            description: page.entry.author !== ""
                ? qsTr("A complete look of the shell and the apps. Original author: %1").arg(page.entry.author)
                : qsTr("A complete look of the shell and the apps")
            trailing: Choice {
                model: page.choices
                currentIndex: Math.max(0, page.previewedIndex)
                onChosen: index => page.previewed = page.choices[index].name
            }
        }
        // Where you are: the theme in use, or a preview to apply or cancel.
        Card {
            icon: page.inUse ? "dialog-ok" : "view-preview"
            title: page.inUse ? qsTr("%1 is in use").arg(page.nameOf(Prefs.visualTheme))
                              : qsTr("Previewing %1").arg(page.entry.text)
            description: page.inUse
                ? (page.resolved.installed
                    ? qsTr("Choose another theme to see it in the preview. Nothing changes until you apply it")
                    : qsTr("\"%1\" isn't installed: Vela's own look is used").arg(Prefs.visualTheme))
                : qsTr("Only the preview shows it. Apply to use it in the shell and the apps instead of %1").arg(page.nameOf(Prefs.visualTheme))
            trailing: Row {
                visible: !page.inUse
                spacing: 8
                Button {
                    text: qsTr("Cancel")
                    onClicked: page.previewed = Prefs.visualTheme
                }
                Button {
                    text: qsTr("Apply")
                    accent: true
                    usable: page.resolved.installed
                    onClicked: Prefs.visualTheme = page.previewed
                }
            }
        }
        Card {
            visible: page.ownAccent
            icon: "preferences-desktop-color"
            title: qsTr("This theme has its own accent color")
            description: qsTr("While it is in use, it replaces the accent color chosen in Colors")
        }
        Card {
            visible: page.resolved.problems.length > 0
            icon: "dialog-warning"
            title: qsTr("This theme has problems")
            description: qsTr("Vela uses what it can and skips the rest")
            contentItem: Column {
                x: 48
                width: parent.width - 64
                spacing: 2
                bottomPadding: 12
                Repeater {
                    model: page.resolved.problems
                    Text {
                        required property var modelData
                        width: parent.width
                        text: page.describe(modelData)
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                    }
                }
            }
        }
    }

    CardGroup {
        Card {
            icon: "configure"
            title: qsTr("Customize")
            description: qsTr("Optional: a style of its own for the taskbar, the Start menu or quick settings, over the theme")
            trailing: Button {
                text: page.customizing ? qsTr("Hide") : qsTr("Customize")
                onClicked: page.customizing = !page.customizing
            }
        }
    }

    // The surfaces' styles, one surface at a time, with their pictures. A
    // style applies at once: it is the user's own choice, saved apart from the
    // theme.
    property bool customizing: Object.values(Prefs.surfaceStyles).some(s => s !== "")
    property string surface: "taskbar"
    readonly property var surfaces: [
        { surface: "taskbar", text: qsTr("Taskbar") },
        { surface: "startmenu", text: qsTr("Start menu") },
        { surface: "notifications", text: qsTr("Quick settings") }
    ]
    // Read again when the surface changes, a style is chosen or a file changes.
    readonly property var gallery: { Prefs.surfaceStyles; Prefs.themesRevision; return Prefs.styleGallery(surface) }
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
            title: qsTr("From the theme")
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
                description: modelData.author ? qsTr("Original author: %1").arg(modelData.author) : ""
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
            icon: "edit-entry"
            title: qsTr("Your own themes")
            description: qsTr("A file in ~/.config/vela/themes/<name>.conf appears here. Start it with inherits=vela, then set only the colors and corners you want to change")
        }
        Card {
            icon: "edit-undo"
            title: qsTr("Reset")
            description: qsTr("Vela Official and no styles. Light or dark mode and the accent stay")
            trailing: Button {
                text: qsTr("Reset")
                usable: Prefs.visualTheme !== "vela" || Object.values(Prefs.surfaceStyles).some(s => s !== "")
                onClicked: Prefs.resetThemes()
            }
        }
    }
}
