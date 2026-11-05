// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization: the desktop preview at the top, the themes to apply with
// a click, then the sections.
Page {
    DesktopPreview {}

    CardGroup {
        title: qsTr("Select a theme to apply")
        ThemeGallery {}
    }

    CardGroup {
        LinkCard {
            icon: "preferences-desktop-wallpaper"
            title: qsTr("Background")
            description: qsTr("Desktop background picture, desktop icons")
            onClicked: root.navigate("background")
        }
        LinkCard {
            icon: "preferences-desktop-color"
            title: qsTr("Colors")
            description: qsTr("Accent color, light or dark mode for apps")
            onClicked: root.navigate("colors")
        }
        LinkCard {
            icon: "preferences-desktop-theme"
            title: qsTr("Themes")
            description: qsTr("Installed themes, your own, back to Vela's own look")
            onClicked: root.navigate("themes")
        }
        LinkCard {
            icon: "start-here"
            title: qsTr("Start")
            description: qsTr("Style of the Start menu")
            onClicked: root.navigate("start")
        }
        LinkCard {
            icon: "preferences-system-windows"
            title: qsTr("Taskbar")
            description: qsTr("Taskbar items and behaviors, styles of the taskbar and quick settings")
            onClicked: root.navigate("taskbar")
        }
    }
}
