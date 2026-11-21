// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Keyboard > Typing: key repeat (docs/keyboard.md §4).
Page {
    CardGroup {
        title: qsTr("Key repeat")
        Card {
            icon: "chronometer"
            title: qsTr("Repeat delay")
            description: qsTr("How long to hold a key before it repeats: ") + Prefs.repeatDelay + " ms"
            trailing: Slider {
                width: 220
                from: 150
                to: 1000
                stepSize: 50
                live: false
                value: Prefs.repeatDelay
                onReleased: value => Prefs.repeatDelay = Math.round(value)
            }
        }
        Card {
            icon: "chronometer"
            title: qsTr("Repeat rate")
            description: Prefs.repeatRate + qsTr(" characters per second")
            trailing: Slider {
                width: 220
                from: 5
                to: 60
                stepSize: 1
                live: false
                value: Prefs.repeatRate
                onReleased: value => Prefs.repeatRate = Math.round(value)
            }
        }
        Card {
            icon: "edit-entry"
            title: qsTr("Test")
            trailing: TextBox {
                width: 260
                placeholderText: qsTr("Hold a key down here")
            }
        }
    }
}
