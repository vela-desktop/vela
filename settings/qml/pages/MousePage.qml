// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Bluetooth & devices > Mouse, like Windows 11: primary button, pointer speed,
// "Enhance pointer precision", lines per wheel notch, middle-click
// autoscroll. The compositor applies them (vela.conf).
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
        Card {
            icon: "transform-move"
            title: qsTr("Autoscroll with the middle button")
            description: qsTr("Move the mouse away from where you pressed the wheel to scroll that way. Terminals keep pasting")
            trailing: Toggle {
                checked: Prefs.middleClickAutoscroll
                onToggled: on => Prefs.middleClickAutoscroll = on
            }
        }
        Card {
            visible: Prefs.middleClickAutoscroll
            icon: "input-mouse"
            title: qsTr("Start with a middle click (instead of new tab or paste)")
            description: qsTr("Click again or press Esc to stop")
            trailing: Toggle {
                checked: Prefs.autoscrollClick
                onToggled: on => Prefs.autoscrollClick = on
            }
        }
        Card {
            visible: Prefs.middleClickAutoscroll
            icon: "input-mouse"
            title: qsTr("Scroll by holding and dragging")
            description: qsTr("Stops when you let go of the button")
            trailing: Toggle {
                checked: Prefs.autoscrollHold
                onToggled: on => Prefs.autoscrollHold = on
            }
        }
        Card {
            visible: Prefs.middleClickAutoscroll
            icon: "input-mouse"
            title: qsTr("Autoscroll speed")
            trailing: Slider {
                width: 220
                from: 1
                to: 10
                stepSize: 1
                live: false
                value: Prefs.autoscrollSpeed
                onReleased: value => Prefs.autoscrollSpeed = Math.round(value)
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
