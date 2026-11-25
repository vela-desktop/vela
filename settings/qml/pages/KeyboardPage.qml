// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Keyboard: the section's pages (docs/keyboard.md §4).
Page {
    CardGroup {
        LinkCard {
            objectName: "layoutsLink"
            icon: "input-keyboard"
            title: qsTr("Layouts")
            description: qsTr("The keyboard layouts you type with, and Win+Space between them")
            onClicked: root.navigate("keyboard-layouts")
        }
        LinkCard {
            objectName: "typingLink"
            icon: "edit-entry"
            title: qsTr("Typing")
            description: qsTr("Key repeat, Caps Lock, Compose and the keypad")
            onClicked: root.navigate("keyboard-typing")
        }
        LinkCard {
            objectName: "shortcutsLink"
            icon: "preferences-desktop-keyboard-shortcuts"
            title: qsTr("Custom shortcuts")
            description: qsTr("Your own key combinations to open apps, commands, links and files")
            onClicked: root.navigate("keyboard-shortcuts")
        }
    }
}
