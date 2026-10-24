// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// One display's brightness (docs/brightness.md §8): its slider, locked while
// the display isn't responding, with a line when it kept its value; or, with
// no slider, once, why there is none. Nothing while the display is probed,
// is off, or has no bus to ask.
Card {
    id: card
    // The display's entry in Brightness.outputs, or undefined.
    property var brightness
    signal moved(real value)

    readonly property bool slider: brightness !== undefined && brightness.available
    readonly property string reason: brightness !== undefined ? brightness.reason : ""
    readonly property var explanations: ({
        "no-answer": qsTr("Brightness is controlled by the display's buttons. If its menu has a DDC/CI option, turning it on lets Vela control it."),
        "unsupported": qsTr("This display doesn't allow changing its brightness"),
        "no-access": qsTr("Vela can't reach this display. The installation may be incomplete")
    })

    visible: slider || explanations[reason] !== undefined
    icon: "brightness-high"
    title: qsTr("Brightness")
    description: !slider ? explanations[reason] ?? ""
        : brightness.locked ? qsTr("This display isn't responding")
        : brightness.refused ? qsTr("The display didn't accept the change")
        : qsTr("Adjust the brightness of this display")
    trailing: Slider {
        visible: card.slider
        width: 200
        usable: card.slider && !card.brightness.locked
        value: card.slider ? card.brightness.value : 0
        Accessible.name: card.title
        onMoved: card.moved(value)
    }
}
