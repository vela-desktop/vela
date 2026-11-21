// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Keyboard > Typing: key repeat, what the Caps Lock, Compose, bottom-left
// and keypad keys do, Num Lock at startup, the Caps Lock display and the
// Win key alone (docs/keyboard.md §4, §6, §7, §8.3, §10).
Page {
    id: page
    // The layouts in use: Settings', or the system's.
    readonly property var layoutsInUse: Prefs.keyboardLayouts.length > 0 ? Prefs.keyboardLayouts : Layouts.source().layouts
    readonly property bool altGrInUse: layoutsInUse.some(l => Preview.typesOnAltGr(l.layout, l.variant))
    readonly property string customText: qsTr("Custom (vela.conf)")

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

    CardGroup {
        title: qsTr("Keys")
        Card {
            icon: "input-keyboard"
            title: qsTr("Caps Lock key")
            description: Prefs.composeKey === "caps" ? qsTr("Used as the Compose key")
                : Prefs.capsKey === "custom" ? qsTr("An option in vela.conf changes this key")
                : ""
            trailing: KeyChoice {
                objectName: "capsKey"
                values: ["capslock", "ctrl", "escape", "backspace", "off"]
                model: [qsTr("Caps Lock"), qsTr("Ctrl"), qsTr("Esc"), qsTr("Backspace"), qsTr("Off")]
                value: Prefs.capsKey
                usable: Prefs.composeKey !== "caps"
                onPicked: value => Prefs.capsKey = value
            }
        }
        Card {
            icon: "input-keyboard"
            title: qsTr("Compose key")
            description: Prefs.composeKey === "ralt" && page.altGrInUse
                ? qsTr("Your layout types characters with AltGr, which is Right Alt: they need another AltGr key now.")
                : qsTr("Press it, then two keys, to type a character such as é or ©")
            trailing: KeyChoice {
                objectName: "composeKey"
                values: ["none", "ralt", "rwin", "menu", "rctrl", "caps", "sclk"]
                model: [qsTr("None"), qsTr("Right Alt"), qsTr("Right Win"), qsTr("Menu"), qsTr("Right Ctrl"), qsTr("Caps Lock"), qsTr("Scroll Lock")]
                value: Prefs.composeKey
                onPicked: value => Prefs.composeKey = value
            }
        }
        Card {
            icon: "input-keyboard"
            title: qsTr("Bottom-left keys")
            description: qsTr("The left Ctrl, Win and Alt; the right ones stay as they are")
            trailing: KeyChoice {
                objectName: "cornerKeys"
                values: ["standard", "ctrl-alt", "alt-win"]
                model: [qsTr("Standard"), qsTr("Swap Ctrl and Alt"), qsTr("Swap Alt and Win (Mac keyboards)")]
                value: Prefs.cornerKeys
                onPicked: value => Prefs.cornerKeys = value
            }
        }
        Card {
            icon: "input-keyboard"
            title: qsTr("Numeric keypad always types digits")
            description: Prefs.numpadKeys === "custom" ? qsTr("An option in vela.conf changes the keypad")
                : qsTr("Whatever Num Lock says, like on a Mac")
            trailing: Switch {
                objectName: "numpadDigits"
                checked: Prefs.numpadKeys === "digits"
                onToggled: Prefs.numpadKeys = checked ? "digits" : "default"
            }
        }
        Card {
            icon: "input-keyboard"
            title: qsTr("Num Lock when Vela starts")
            description: Prefs.numpadKeys === "digits" ? qsTr("The keypad always types digits, whatever Num Lock says")
                : qsTr("Every keyboard shares Num Lock and Caps Lock")
            trailing: KeyChoice {
                objectName: "numLock"
                values: ["off", "on", "last"]
                model: [qsTr("Off", "Num Lock"), qsTr("On", "Num Lock"), qsTr("As it was", "Num Lock")]
                value: Prefs.numLock
                usable: Prefs.numpadKeys !== "digits"
                onPicked: value => Prefs.numLock = value
            }
        }
        Card {
            icon: "input-keyboard"
            title: qsTr("Show Caps Lock on screen")
            description: qsTr("For a moment, each time Caps Lock turns on or off")
            trailing: Switch {
                objectName: "capsLockOsd"
                checked: Prefs.capsLockOsd
                onToggled: Prefs.capsLockOsd = checked
            }
        }
        Card {
            icon: "start-here"
            title: qsTr("Open Start with the Win key")
            description: qsTr("Pressing the Win key by itself opens Start; Win shortcuts work either way")
            trailing: Switch {
                objectName: "superOpensStart"
                checked: Prefs.superOpensStart
                onToggled: Prefs.superOpensStart = checked
            }
        }
    }

    // A drop-down of a key control: `values` matched to the model's texts,
    // "custom" shown as such until something else is picked.
    component KeyChoice: Choice {
        id: choice
        property var values: []
        property string value
        signal picked(string value)
        currentIndex: values.indexOf(value)
        displayText: currentIndex >= 0 ? model[currentIndex] : page.customText
        // Wide enough for "Custom (vela.conf)" too.
        implicitWidth: Math.max(160, Math.min(320, Math.max(contentWidth, customWidth.advanceWidth) + 56))
        TextMetrics {
            id: customWidth
            font: choice.font
            text: page.customText
        }
        onChosen: index => choice.picked(values[index])
    }
}
