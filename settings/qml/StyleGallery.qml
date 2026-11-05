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
    readonly property string themeName: Prefs.visualThemes().find(t => t.name === Prefs.visualTheme)?.text ?? Prefs.visualTheme
    // The pictures' shape (scripts/theme-pictures.py): a taskbar's is the
    // bottom of a 1280 x 720 desktop, a strip as wide as a tile in one
    // column; the Start menu's is tall, quick settings' two side by side.
    readonly property real minimumTile: surface === "taskbar" ? 600 : 220
    readonly property real pictureHeight: surface === "taskbar" ? Math.round((tileWidth - 12) * 96 / 1280) : 220
    readonly property real tileWidth: (width - grid.spacing * (grid.columns - 1)) / grid.columns

    width: parent ? parent.width : 0
    spacing: 12

    // One stop for Tab in the gallery, on the style in use (From the theme
    // first) or where the arrows last went; the arrows move, Space applies.
    property int focusIndex: -1
    readonly property int count: styles.length + 1
    readonly property int tabIndex: focusIndex >= 0 && focusIndex < count ? focusIndex
        : chosen === "" || missing ? 0 : styles.findIndex(s => s.name === chosen) + 1
    function tileAt(i) {
        return i === 0 ? themeTile : styleTiles.itemAt(i - 1)
    }
    function moveFocus(from, step) {
        const to = from + step
        if (to >= 0 && to < count) {
            tileAt(to).forceActiveFocus(Qt.TabFocusReason)
        }
    }

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

        PictureTile {
            id: themeTile
            // The theme's pictures, or those of a theme it builds on.
            readonly property var pictures: { Prefs.themesRevision; return Prefs.themeSurfacePictures(Prefs.visualTheme, gallery.surface) }
            readonly property var images: pictures.images || []
            readonly property string from: pictures.from || ""
            width: gallery.tileWidth
            pictureHeight: gallery.pictureHeight
            tabStop: gallery.tabIndex === 0
            onActiveFocusChanged: if (activeFocus) gallery.focusIndex = 0
            onArrow: (dx, dy) => gallery.moveFocus(0, dx + dy * grid.columns)
            text: qsTr("From the theme")
            subtitle: from !== "" && from !== Prefs.visualTheme
                ? qsTr("As %1 draws it, based on %2").arg(gallery.themeName)
                    .arg(Prefs.visualThemes().find(t => t.name === from)?.text ?? from)
                : qsTr("As %1 draws it").arg(gallery.themeName)
            selected: gallery.chosen === "" || gallery.missing
            onClicked: gallery.use("")
            Pictures {
                images: themeTile.images
            }
            // No picture in the theme's chain: its icon.
            Icon {
                visible: themeTile.images.length === 0
                anchors.centerIn: parent
                name: "preferences-desktop-theme"
                size: 32
                opacity: 0.6
            }
        }

        Repeater {
            id: styleTiles
            model: gallery.styles
            PictureTile {
                id: tile
                required property var modelData
                required property int index
                tabStop: gallery.tabIndex === index + 1
                onActiveFocusChanged: if (activeFocus) gallery.focusIndex = index + 1
                onArrow: (dx, dy) => gallery.moveFocus(index + 1, dx + dy * grid.columns)
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
