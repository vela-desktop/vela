// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Bluetooth & devices > Touchpad, like Windows 11: on or off (also only when
// there's a mouse), speed, taps, scroll direction and three- and four-finger
// gestures. The compositor applies them.
Page {
    id: page
    readonly property var actions: ["app", "desktop", "no"]
    readonly property var actionNames: [qsTr("Switch apps and show the desktop"), qsTr("Switch desktops and show the desktop"), qsTr("Nothing")]

    Card {
        visible: !Prefs.hasTouchpad
        icon: "input-touchpad"
        title: qsTr("No touchpad")
        description: qsTr("These settings apply when there's a touchpad, for example on a laptop.")
    }

    CardGroup {
        Card {
            icon: "input-touchpad"
            title: qsTr("Touchpad")
            trailing: Switch {
                checked: Prefs.touchpad
                onToggled: Prefs.touchpad = checked
            }
        }
        Card {
            icon: "input-mouse"
            title: qsTr("Leave the touchpad on when a mouse is connected")
            trailing: Switch {
                checked: Prefs.touchpadWithMouse
                onToggled: Prefs.touchpadWithMouse = checked
            }
        }
        Card {
            icon: "transform-move"
            title: qsTr("Cursor speed")
            trailing: Slider {
                width: 220
                from: 1
                to: 20
                stepSize: 1
                live: false
                value: Prefs.touchpadSpeed
                onReleased: value => Prefs.touchpadSpeed = Math.round(value)
            }
        }
    }

    CardGroup {
        title: qsTr("Taps and scrolling")
        Card {
            icon: "input-touchpad"
            title: qsTr("Tap with a single finger to single-click")
            description: qsTr("Two fingers for a right-click, tap and drag to move things")
            trailing: Switch {
                checked: Prefs.touchpadTap
                onToggled: Prefs.touchpadTap = checked
            }
        }
        Card {
            icon: "transform-move-vertical"
            title: qsTr("Scrolling direction")
            description: qsTr("With two fingers: moving them down")
            trailing: Choice {
                model: [qsTr("Content goes up"), qsTr("Content goes down")]
                currentIndex: Prefs.touchpadNatural ? 0 : 1
                onChosen: index => Prefs.touchpadNatural = index === 0
            }
        }
    }

    CardGroup {
        title: qsTr("Gestures")
        Card {
            icon: "gesture"
            title: qsTr("Three-finger gestures")
            description: qsTr("Up: Task view; down: the desktop; sideways:")
            trailing: Choice {
                model: page.actionNames
                currentIndex: Math.max(0, page.actions.indexOf(Prefs.threeFingers))
                onChosen: index => Prefs.threeFingers = page.actions[index]
            }
        }
        Card {
            icon: "gesture"
            title: qsTr("Four-finger gestures")
            description: qsTr("Up: Task view; down: the desktop; sideways:")
            trailing: Choice {
                model: page.actionNames
                currentIndex: Math.max(0, page.actions.indexOf(Prefs.fourFingers))
                onChosen: index => Prefs.fourFingers = page.actions[index]
            }
        }
    }
}
