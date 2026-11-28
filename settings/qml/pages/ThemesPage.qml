// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 Lowell137
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Themes: a theme file (theme= in vela.conf), a complete
// look of the shell and the apps (docs/configuration.md, "Theme files").
// What is in use, layer by layer; a click on a theme applies it, with Undo
// for a few seconds; what is wrong in
// the theme in use; your own themes in ~/.config/vela/themes/; Reset. Each
// surface's own style over the theme is on that surface's page (Taskbar,
// Start).
Page {
    id: page
    // Settings' Preferences, named once: qmllint can check page.prefs.
    readonly property var prefs: Prefs
    property string fontFilter
    readonly property var visibleFonts: fontFilter.trim() === "" ? page.prefs.systemFonts
        : page.prefs.systemFonts.filter(name => name.toLowerCase().includes(fontFilter.trim().toLowerCase()))
    // The theme in use, as the shell takes it (themefile.h): its tokens and
    // what is wrong in it; again when a theme file changes (themesRevision).
    readonly property var resolved: { page.prefs.visualTheme; page.prefs.themesRevision; return page.prefs.resolveTheme(page.prefs.visualTheme) }

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
        case "too-large": what = qsTr("larger than 64 KiB: the file isn't used"); break
        default: what = problem.kind
        }
        const file = problem.file.substring(problem.file.lastIndexOf("/") + 1)
        return (problem.line > 0 ? file + ":" + problem.line : file) + ": " + what
    }

    // What is in use, layer by layer, in place of a schematic picture: the
    // gallery below shows the theme itself.
    CardGroup {
        ThemeInUseCard {}
    }

    CardGroup {
        ThemeGallery {}
    }

    CardGroup {
        Card {
            visible: !page.resolved.installed
            icon: "dialog-warning"
            title: qsTr("\"%1\" isn't installed").arg(page.prefs.visualTheme)
            description: qsTr("vela.conf names a theme Vela can't find: Vela's own look is used")
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
        OwnThemesCard {}
        ResetThemesCard {}
    }

    CardGroup {
        title: qsTr("System appearance")
        Card {
            icon: "preferences-desktop-font"
            title: qsTr("System font")
            description: qsTr("Font used by KDE and Qt applications")
            trailing: Choice {
                width: 220
                usable: page.visibleFonts.length > 0
                model: page.visibleFonts
                currentIndex: page.visibleFonts.indexOf(page.prefs.systemFont)
                onChosen: index => page.prefs.systemFont = page.visibleFonts[index]
            }
            contentItem: TextBox {
                width: parent.width
                placeholderText: qsTr("Find a font")
                text: page.fontFilter
                onTextChanged: page.fontFilter = text
            }
        }
        Card {
            icon: "preferences-desktop-icons"
            title: qsTr("Icon theme")
            description: qsTr("Icons used across KDE and Qt applications")
            trailing: Choice {
                usable: page.prefs.iconThemes.length > 0
                model: page.prefs.iconThemes
                currentIndex: page.prefs.iconThemes.indexOf(page.prefs.iconTheme)
                onChosen: index => page.prefs.iconTheme = page.prefs.iconThemes[index]
            }
        }
        Card {
            icon: "preferences-desktop-cursor"
            title: qsTr("Cursor theme")
            description: qsTr("Pointer style used across the desktop")
            trailing: Choice {
                usable: page.prefs.cursorThemes.length > 0
                model: page.prefs.cursorThemes
                currentIndex: page.prefs.cursorThemes.indexOf(page.prefs.cursorTheme)
                onChosen: index => page.prefs.cursorTheme = page.prefs.cursorThemes[index]
            }
        }
        Card {
            icon: "preferences-desktop-cursor"
            title: qsTr("Cursor size")
            description: qsTr("In pixels at 100% scale, one of the sizes the cursor theme is drawn at")
            trailing: CursorSizeField {
                prefs: page.prefs
            }
        }
    }
}
