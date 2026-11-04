// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes
import Vela.Controls

// Vela's symbol: a small stylized sail. It's the Start button.
Item {
    id: root
    width: 22
    height: 22

    Shape {
        anchors.fill: parent
        // Soft antialiasing on the slanted edges too.
        layer.enabled: true
        layer.samples: 4

        // Big sail
        ShapePath {
            strokeWidth: 0
            strokeColor: "transparent"
            fillGradient: LinearGradient {
                x1: 12; y1: 1
                x2: 21; y2: 16
                GradientStop { position: 0; color: Theme.accentFill }
                GradientStop { position: 1; color: Theme.accent }
            }
            startX: 12; startY: 1
            PathLine { x: 21; y: 16 }
            PathLine { x: 12; y: 16 }
            PathLine { x: 12; y: 1 }
        }

        // Small sail
        ShapePath {
            strokeWidth: 0
            strokeColor: "transparent"
            fillColor: Theme.accentFill
            startX: 10; startY: 5
            PathLine { x: 10; y: 16 }
            PathLine { x: 3; y: 16 }
            PathLine { x: 10; y: 5 }
        }

        // Hull
        ShapePath {
            strokeWidth: 0
            strokeColor: "transparent"
            fillColor: Theme.accent
            startX: 1; startY: 18
            PathLine { x: 21; y: 18 }
            PathLine { x: 17.5; y: 21.5 }
            PathLine { x: 4.5; y: 21.5 }
            PathLine { x: 1; y: 18 }
        }
    }
}
