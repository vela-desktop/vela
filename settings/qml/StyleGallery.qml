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
    // vela.conf names a style that isn't there (removed, renamed): the shell
    // draws the theme's look, and the gallery says so.
    readonly property bool missing: chosen !== "" && !styles.some(s => s.name === chosen)
    // The cards: From the theme ("", the theme's pictures or those of a theme
    // it builds on), then the styles.
    readonly property var entries: {
        Prefs.themesRevision
        const theme = Prefs.themeSurfacePictures(Prefs.visualTheme, surface)
        const name = Prefs.displayName("", Prefs.visualTheme) || Prefs.visualTheme
        const from = theme.from || Prefs.visualTheme
        return [{
            name: "",
            text: qsTr("From the theme"),
            subtitle: from === Prefs.visualTheme ? qsTr("As %1 draws it").arg(name)
                : qsTr("As %1 draws it, based on %2").arg(name).arg(Prefs.displayName("", from) || from),
            images: theme.images || []
        }].concat(styles.map(s => ({
            name: s.name,
            text: s.text,
            subtitle: s.author ? qsTr("Original author: %1").arg(s.author) : "",
            images: s.images
        })))
    }
    // The pictures' shape (scripts/theme-pictures.py): a taskbar's is the
    // bottom of a 1280 x 720 desktop, a strip as wide as a tile in one
    // column; the Start menu's is tall, quick settings' two side by side.
    readonly property real minimumTile: surface === "taskbar" ? 600 : 220
    readonly property real pictureHeight: surface === "taskbar" ? Math.round((tileWidth - 12) * 96 / 1280) : 220
    readonly property real tileWidth: (width - grid.spacing * (grid.columns - 1)) / grid.columns

    width: parent ? parent.width : 0
    spacing: 12

    // The arrows move between cards (PictureTile), Space applies one.
    function moveFocus(from, step) {
        const to = from + step
        if (to >= 0 && to < entries.length) {
            tiles.itemAt(to).forceActiveFocus(Qt.TabFocusReason)
        }
    }
    function use(style) {
        const previous = chosen
        if (style === previous) {
            return
        }
        Prefs.setSurfaceStyle(surface, style)
        const surface_ = surface
        root.offerUndo(style === "" ? qsTr("The theme's style is in use") : qsTr("%1 is in use").arg(Prefs.displayName(surface, style) || style),
            () => Prefs.setSurfaceStyle(surface_, previous))
    }

    Card {
        objectName: "missingStyle"
        visible: gallery.missing
        icon: "dialog-warning"
        title: qsTr("\"%1\" isn't installed").arg(gallery.chosen)
        description: qsTr("vela.conf names a style Vela can't find, so the theme's look is in use. Choose one below")
    }

    Grid {
        id: grid
        width: parent.width
        columns: Math.max(1, Math.min(4, Math.floor((width + spacing) / (gallery.minimumTile + spacing))))
        spacing: 8

        Repeater {
            id: tiles
            model: gallery.entries
            PictureTile {
                id: tile
                required property var modelData
                required property int index
                width: gallery.tileWidth
                pictureHeight: gallery.pictureHeight
                text: modelData.text
                subtitle: modelData.subtitle
                // From the theme while what vela.conf names isn't there.
                selected: modelData.name === "" ? gallery.chosen === "" || gallery.missing : gallery.chosen === modelData.name
                tabStop: selected
                onArrow: (dx, dy) => gallery.moveFocus(index, dx + dy * grid.columns)
                onClicked: gallery.use(modelData.name)

                // Side by side, centered and whole; a style without a
                // picture (your own) shows its name only.
                Row {
                    id: pictures
                    readonly property int count: Math.max(1, tile.modelData.images.length)
                    anchors.centerIn: parent
                    spacing: 6
                    Repeater {
                        model: tile.modelData.images
                        Image {
                            required property string modelData
                            width: (tile.width - 12 - pictures.spacing * (pictures.count - 1)) / pictures.count
                            height: tile.pictureHeight
                            fillMode: Image.PreserveAspectFit
                            source: modelData
                            sourceSize.height: height * 2
                            asynchronous: true
                            smooth: true
                        }
                    }
                }
                // From the theme without a picture in the theme's chain: its icon.
                Icon {
                    visible: tile.modelData.name === "" && tile.modelData.images.length === 0
                    anchors.centerIn: parent
                    name: "preferences-desktop-theme"
                    size: 32
                    opacity: 0.6
                }
            }
        }
    }

    // Where your own styles go, and the folder itself.
    Item {
        width: parent.width
        height: Math.max(ownStyles.height, openFolder.height)
        Text {
            id: ownStyles
            anchors { left: parent.left; right: openFolder.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
            text: qsTr("Your own styles: a file in ~/.config/vela/themes/%1/<name>.conf appears here").arg(gallery.surface)
            textFormat: Text.PlainText
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        Button {
            id: openFolder
            objectName: "openFolder"
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            text: qsTr("Open folder")
            onClicked: Prefs.openThemesFolder(gallery.surface)
        }
    }
}
