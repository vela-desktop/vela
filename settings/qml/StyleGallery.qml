// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 Lowell137
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A surface's styles (taskbar-style=, startmenu-style=, notifications-style=
// in vela.conf; themes/<surface>/), as cards with their pictures, after "From
// the theme": the surface as the theme in use draws it. A click applies a
// style at once, saved apart from the theme, and Undo goes back to the one
// before (UndoBar). Under them, where your own styles go.
Column {
    id: gallery
    // "taskbar", "startmenu" or "notifications".
    required property string surface
    // Read again when a file changes.
    readonly property var styles: { Prefs.themesRevision; return Prefs.styleGallery(surface) }
    readonly property string chosen: Prefs.surfaceStyles[surface] || ""
    readonly property string themeName: Prefs.visualThemes().find(t => t.name === Prefs.visualTheme)?.text ?? Prefs.visualTheme
    // The pictures' shape (scripts/theme-pictures.py): a taskbar's is the
    // bottom of a 1280 x 720 desktop, a strip as wide as a tile in one
    // column; the Start menu's is tall, quick settings' two side by side.
    readonly property real minimumTile: surface === "taskbar" ? 600 : 220
    readonly property real pictureHeight: surface === "taskbar" ? Math.round((tileWidth - 12) * 96 / 1280) : 220
    readonly property real tileWidth: (width - grid.spacing * (grid.columns - 1)) / grid.columns

    width: parent ? parent.width : 0
    spacing: 12

    function nameOf(style) {
        const entry = styles.find(s => s.name === style)
        return entry ? entry.text : style
    }
    function use(style) {
        const previous = chosen
        if (style === previous) {
            return
        }
        Prefs.setSurfaceStyle(surface, style)
        const surface_ = surface
        root.offerUndo(style === "" ? qsTr("The theme's style is in use") : qsTr("%1 is in use").arg(nameOf(style)),
            () => Prefs.setSurfaceStyle(surface_, previous))
    }

    Grid {
        id: grid
        width: parent.width
        columns: Math.max(1, Math.min(4, Math.floor((width + spacing) / (gallery.minimumTile + spacing))))
        spacing: 8

        PictureTile {
            id: themeTile
            readonly property var images: { Prefs.themesRevision; return Prefs.themeSurfacePictures(Prefs.visualTheme, gallery.surface) }
            width: gallery.tileWidth
            pictureHeight: gallery.pictureHeight
            text: qsTr("From the theme")
            subtitle: qsTr("As %1 draws it").arg(gallery.themeName)
            selected: gallery.chosen === ""
            onClicked: gallery.use("")
            Pictures {
                images: themeTile.images
            }
            // A theme without pictures (your own): its icon.
            Icon {
                visible: themeTile.images.length === 0
                anchors.centerIn: parent
                name: "preferences-desktop-theme"
                size: 32
                opacity: 0.6
            }
        }

        Repeater {
            model: gallery.styles
            PictureTile {
                id: tile
                required property var modelData
                width: gallery.tileWidth
                pictureHeight: gallery.pictureHeight
                text: modelData.text
                subtitle: modelData.author ? qsTr("Original author: %1").arg(modelData.author) : ""
                selected: gallery.chosen === modelData.name
                onClicked: gallery.use(modelData.name)

                // A style without a picture (your own) shows its name only.
                Pictures {
                    images: tile.modelData.images
                }
            }
        }
    }

    Text {
        width: parent.width
        text: qsTr("Your own styles: a file in ~/.config/vela/themes/%1/<name>.conf appears here").arg(gallery.surface)
        textFormat: Text.PlainText
        color: Theme.textSecondary
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
    }

    // A tile's pictures side by side, centered and whole.
    component Pictures: Row {
        id: pictures
        property var images: []
        readonly property int count: Math.max(1, images.length)
        readonly property real each: parent ? (parent.width - spacing * (count - 1)) / count : 0
        readonly property real tall: parent ? parent.height : 0
        anchors.centerIn: parent
        spacing: 6
        Repeater {
            model: pictures.images
            Image {
                required property string modelData
                width: pictures.each
                height: pictures.tall
                fillMode: Image.PreserveAspectFit
                source: modelData
                sourceSize.height: height * 2
                asynchronous: true
                smooth: true
            }
        }
    }
}
