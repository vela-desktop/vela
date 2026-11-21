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
            description: qsTr("Key repeat")
            onClicked: root.navigate("keyboard-typing")
        }
    }
}
