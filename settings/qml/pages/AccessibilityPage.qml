// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Accessibility, like Windows 11: animation effects, the magnifier, color
// filters (also with Win+Ctrl+C) and sticky keys. The compositor applies them
// (compositor/src/a11y.c); animation effects reach every Vela process too
// (Theme.animationEffects).
Page {
    id: page
    readonly property var steps: [25, 50, 100, 150, 200, 400]
    readonly property int suggestedDuration: 100 // percent: Vela's durations
    readonly property var filters: [
        { value: "grayscale", text: qsTr("Grayscale") },
        { value: "deuteranopia", text: qsTr("Red-green (green weak, deuteranopia)") },
        { value: "protanopia", text: qsTr("Red-green (red weak, protanopia)") },
        { value: "tritanopia", text: qsTr("Blue-yellow (tritanopia)") }
    ]

    CardGroup {
        title: qsTr("Visual effects")
        Card {
            icon: "preferences-desktop-effects"
            title: qsTr("Animation effects")
            description: qsTr("Windows and panels move as they open, close and change. Off, they only fade, briefly")
            trailing: Switch {
                checked: Prefs.animationEffects
                onToggled: Prefs.animationEffects = checked
            }
        }
        Card {
            icon: "chronometer"
            title: qsTr("Animation duration")
            description: qsTr("How long they take, compared with Vela's suggested durations (the mark). Double-click the knob to go back to them")
            enabled: Prefs.animationEffects
            opacity: enabled ? 1 : 0.5
            trailing: Row {
                spacing: 12
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: durationSlider.value === page.suggestedDuration
                        ? qsTr("%1% (suggested)").arg(Math.round(durationSlider.value))
                        : qsTr("%1%").arg(Math.round(durationSlider.value))
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                Slider {
                    id: durationSlider
                    anchors.verticalCenter: parent.verticalCenter
                    width: 220
                    from: 50
                    to: 300
                    stepSize: 25
                    mark: page.suggestedDuration
                    resetValue: page.suggestedDuration
                    live: false
                    value: Prefs.animationDuration
                    onReleased: value => Prefs.animationDuration = Math.round(value)
                }
            }
        }
    }

    CardGroup {
        title: qsTr("Vision")
        Card {
            icon: "zoom-in"
            title: qsTr("Magnifier")
            description: qsTr("Win+Plus opens the magnifier and zooms in, Win+Minus zooms out, Win+Esc closes it")
            trailing: Switch {
                checked: Prefs.magnifier
                onToggled: Prefs.magnifier = checked
            }
        }
        Card {
            icon: "zoom-in"
            title: qsTr("Zoom increment")
            description: qsTr("How much each press of Win+Plus zooms in")
            trailing: Choice {
                model: page.steps.map(s => s + "%")
                currentIndex: Math.max(0, page.steps.indexOf(Prefs.magnifierStep))
                onChosen: index => Prefs.magnifierStep = page.steps[index]
            }
        }
        Card {
            icon: "preferences-desktop-color"
            title: qsTr("Color filters")
            description: qsTr("To see photos and colors better, or to tell colors apart with color blindness")
            trailing: Switch {
                checked: Prefs.colorFilter
                onToggled: Prefs.colorFilter = checked
            }
        }
        Card {
            icon: "color-management"
            title: qsTr("Filter")
            trailing: Choice {
                model: page.filters.map(f => f.text)
                currentIndex: Math.max(0, page.filters.findIndex(f => f.value === Prefs.colorFilterKind))
                onChosen: index => Prefs.colorFilterKind = page.filters[index].value
            }
        }
        Card {
            icon: "input-keyboard"
            title: qsTr("Keyboard shortcut for color filters")
            description: qsTr("Win+Ctrl+C turns them on and off")
            trailing: Switch {
                checked: Prefs.colorFilterShortcut
                onToggled: Prefs.colorFilterShortcut = checked
            }
        }
    }

    CardGroup {
        title: qsTr("Interaction")
        Card {
            icon: "input-keyboard"
            title: qsTr("Sticky keys")
            description: qsTr("Shift, Ctrl, Alt and Win pressed and released apply to the next key: shortcuts are typed one key at a time. Pressed twice, they stay locked")
            trailing: Switch {
                checked: Prefs.stickyKeys
                onToggled: Prefs.stickyKeys = checked
            }
        }
    }
}
