// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes

// The ">" of what opens a page; `down`: the "v" of what opens a list. A thin
// stroke with round ends, in `color`.
Shape {
    id: chevron
    property color color: Theme.text
    property bool down: false
    width: down ? 9 : 6
    height: down ? 5 : 10
    preferredRendererType: Shape.CurveRenderer
    ShapePath {
        strokeColor: chevron.color
        strokeWidth: 1.2
        fillColor: "transparent"
        capStyle: ShapePath.RoundCap
        joinStyle: ShapePath.RoundJoin
        startX: 1; startY: 1
        PathLine { x: chevron.down ? 4.5 : 5; y: chevron.down ? 4.5 : 5 }
        PathLine { x: chevron.down ? 8 : 1; y: chevron.down ? 1 : 9 }
    }
}
