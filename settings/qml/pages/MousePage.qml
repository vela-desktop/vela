// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Bluetooth & devices > Mouse, like Windows 11: primary button, pointer speed,
// "Enhance pointer precision", lines per wheel notch. The compositor applies
// them (vela.conf).
Page {
    CardGroup {
        Card {
            icon: "input-mouse"
            title: qsTr("Primary mouse button")
            trailing: Choice {
                model: [qsTr("Left"), qsTr("Right")]
                currentIndex: Prefs.mouseLeftHanded ? 1 : 0
                onChosen: index => Prefs.mouseLeftHanded = index === 1
            }
        }
        Card {
            icon: "transform-move"
            title: qsTr("Mouse pointer speed")
            trailing: Slider {
                width: 220
                from: 1
                to: 20
                stepSize: 1
                live: false
                value: Prefs.mouseSpeed
                onReleased: value => Prefs.mouseSpeed = Math.round(value)
            }
        }
        Card {
            icon: "edit-select"
            title: qsTr("Enhance pointer precision")
            description: qsTr("The pointer goes further when you move the mouse faster (acceleration)")
            trailing: Switch {
                checked: Prefs.mousePrecision
                onToggled: Prefs.mousePrecision = checked
            }
        }
    }

    CardGroup {
        title: qsTr("Scrolling")
        Card {
            icon: "input-mouse"
            title: qsTr("Lines to scroll at a time")
            description: qsTr("For each notch of the wheel")
            trailing: Slider {
                width: 220
                from: 1
                to: 20
                stepSize: 1
                live: false
                value: Prefs.wheelLines
                onReleased: value => Prefs.wheelLines = Math.round(value)
            }
        }
    }

    CardGroup {
        title: qsTr("Related settings")
        LinkCard {
            visible: Prefs.hasTouchpad
            icon: "input-touchpad"
            title: qsTr("Touchpad")
            description: qsTr("Taps, gestures, scrolling")
            onClicked: root.navigate("touchpad")
        }
        LinkCard {
            icon: "preferences-desktop-accessibility"
            title: qsTr("Accessibility")
            description: qsTr("Magnifier, color filters, sticky keys")
            onClicked: root.navigate("accessibility")
        }
    }
}
