// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The installed themes (theme= in vela.conf, themes/), the user's among them,
// as cards with their picture: a click makes one the theme in use at once,
// and Undo, for a few seconds, goes back to the one before (UndoBar). The
// picture is the desktop in that theme (themes/screenshots/themes/, made by
// scripts/theme-pictures.py); a theme without one, your own, shows the
// desktop in miniature in its colors and taskbar instead.
Grid {
    id: gallery
    readonly property var choices: { Prefs.themesRevision; return Prefs.styleGallery("") }
    // Tab's stop: the theme in use, the first when it isn't installed.
    readonly property bool noneInUse: !choices.some(c => c.name === Prefs.visualTheme)
    readonly property real tileWidth: (width - spacing * (columns - 1)) / columns

    width: parent ? parent.width : 0
    columns: Math.max(1, Math.min(3, Math.floor((width + spacing) / (200 + spacing))))
    spacing: 8

    // The arrows move between themes (PictureTile), Space applies one.
    function moveFocus(from, step) {
        const to = from + step
        if (to >= 0 && to < choices.length) {
            tiles.itemAt(to).forceActiveFocus(Qt.TabFocusReason)
        }
    }

    function use(entry) {
        const previous = Prefs.visualTheme
        if (entry.name === previous) {
            return
        }
        Prefs.visualTheme = entry.name
        root.offerUndo(qsTr("%1 is in use").arg(entry.text), () => { Prefs.visualTheme = previous })
    }

    Repeater {
        id: tiles
        model: gallery.choices
        PictureTile {
            id: tile
            required property var modelData
            required property int index
            tabStop: selected || (index === 0 && gallery.noneInUse)
            onArrow: (dx, dy) => gallery.moveFocus(index, dx + dy * gallery.columns)
            width: gallery.tileWidth
            // 16:9, the pictures' shape (and the miniature's, unframed).
            pictureHeight: Math.round((width - 12) * 9 / 16)
            text: modelData.text
            subtitle: modelData.author !== "" ? qsTr("Original author: %1").arg(modelData.author) : ""
            selected: modelData.name === Prefs.visualTheme
            onClicked: gallery.use(modelData)

            Image {
                visible: tile.modelData.images.length > 0
                anchors.fill: parent
                source: visible ? tile.modelData.images[0] : ""
                sourceSize.width: width * 2
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                smooth: true
            }
            DesktopPreview {
                visible: tile.modelData.images.length === 0
                framed: false
                // Again when a file or a chosen style changes.
                themeTokens: { Prefs.themesRevision; Prefs.surfaceStyles; return Prefs.resolveTheme(tile.modelData.name).tokens }
                transformOrigin: Item.TopLeft
                scale: tile.pictureHeight / height
            }
        }
    }
}
