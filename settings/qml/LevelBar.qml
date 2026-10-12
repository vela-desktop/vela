// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A sound level, like Windows' input and mixer bars: a thin track, filled
// up to `level` (0-1, a LevelMeter's).
Item {
    id: bar
    property real level: 0
    implicitWidth: 200
    implicitHeight: 4

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Theme.controlStrokeStrong
        opacity: 0.6
    }
    Rectangle {
        width: parent.width * Math.max(0, Math.min(1, bar.level))
        height: parent.height
        radius: height / 2
        color: Theme.accentFill
    }
}
