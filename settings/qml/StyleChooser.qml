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
    // What the closed card says: the style in use.
    readonly property string summary: {
        Prefs.themesRevision
        if (chosen === "") {
            return qsTr("From the theme, as %1 draws it").arg(Prefs.displayName("", Prefs.visualTheme) || Prefs.visualTheme)
        }
        return Prefs.displayName(surface, chosen) || qsTr("\"%1\" isn't installed: the theme's look is in use").arg(chosen)
    }

    width: parent ? parent.width : 0

    // Asked for (Main.qml's reveal: a search for its style, Themes' In use):
    // open, scrolled into view, and with the focus, so Tab goes on from it.
    readonly property string wanted: root.reveal
    onWantedChanged: if (wanted === surface) show()
    Component.onCompleted: if (wanted === surface) show()
    function show() {
        expanded = true
        header.takeFocus()
        scrollLater.restart()
    }
    // Once the opened gallery is laid out, so the page is as long as it gets.
    Timer {
        id: scrollLater
        interval: 50
        onTriggered: chooser.scrollIntoView()
    }
    function scrollIntoView() {
        // The page it is on: the first Flickable up the tree.
        for (let item = chooser.parent; item; item = item.parent) {
            const page = item as Flickable
            if (page) {
                const y = chooser.mapToItem(page.contentItem, 0, 0).y
                page.contentY = Math.max(0, Math.min(y - 16, page.contentHeight - page.height))
                return
            }
        }
    }

    Card {
        id: header
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
