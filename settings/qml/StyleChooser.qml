// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A surface's style as Windows 11's expanders: a card with the style in use,
// that opens the surface's gallery (StyleGallery) under it, and closes it
// again (a click, or Space or Enter). Taskbar keeps its own settings first
// and its styles here, closed, so a long gallery doesn't push them away.
Column {
    id: chooser
    required property string surface
    property string icon
    property string title
    property bool expanded: false
    readonly property string chosen: Prefs.surfaceStyles[surface] || ""
    readonly property var styles: { Prefs.themesRevision; return Prefs.styleGallery(surface) }
    readonly property string themeName: Prefs.visualThemes().find(t => t.name === Prefs.visualTheme)?.text ?? Prefs.visualTheme
    // What the closed card says: the style in use.
    readonly property string summary: {
        if (chosen === "") {
            return qsTr("From the theme, as %1 draws it").arg(themeName)
        }
        const entry = styles.find(s => s.name === chosen)
        return entry ? entry.text : qsTr("\"%1\" isn't installed: the theme's look is in use").arg(chosen)
    }

    width: parent ? parent.width : 0

    Card {
        objectName: "styleHeader"
        icon: chooser.icon
        title: chooser.title
        description: chooser.summary
        clickable: true
        last: !chooser.expanded
        trailing: Chevron {
            anchors.verticalCenter: parent.verticalCenter
            down: true
            rotation: chooser.expanded ? 180 : 0
            color: Theme.textSecondary
        }
        onClicked: chooser.expanded = !chooser.expanded
    }

    Fill {
        objectName: "styleBody"
        visible: chooser.expanded
        width: parent.width
        height: visible ? body.height + 32 : 0
        color: Theme.card
        radius: 0
        bottomLeftRadius: Theme.radiusCard
        bottomRightRadius: Theme.radiusCard
        border.width: 1
        border.color: Theme.cardStroke

        // Made when opened: a closed gallery loads no pictures.
        Loader {
            id: body
            x: 16
            y: 16
            width: parent.width - 32
            active: chooser.expanded
            sourceComponent: StyleGallery {
                surface: chooser.surface
            }
        }
    }
}
