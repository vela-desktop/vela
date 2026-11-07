// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes

// The Start button's icon: Vela's symbol, a small stylized sail in the
// accent's colors; or, as chosen in Settings > Start, the distribution's
// logo or a picture (`source`: an icon theme's name or a file's path,
// shell/src/starticon.h). The taskbar draws it, Settings shows it.
Item {
    id: root
    property string source
    // Goes up when the picture changed under the same name: loaded again.
    property int revision: 0
    width: 22
    height: 22

    Icon {
        id: picture
        visible: root.source !== ""
        anchors.centerIn: parent
        name: root.source
        size: 22
        // A picture of the user's may change under the same name.
        cache: false
    }
    onRevisionChanged: {
        picture.name = ""
        picture.name = Qt.binding(() => root.source)
    }

    Shape {
        visible: root.source === ""
        anchors.fill: parent
        // Soft antialiasing on the slanted edges too.
        layer.enabled: visible
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
