// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Effects

// What floats casts a soft shadow, drawn only *outside* it: acrylic panels are
// semi-transparent and must show the blurred background below, not a shadow.
// RectangularShadow (analytic, like the compositor's) clipped into four strips
// around the target. `depth` is one of Theme.shadow's (flyout, dialog); the
// window must leave `reach` around the target, or the shadow is cut.
Item {
    id: root

    required property Item target
    property var depth: Theme.shadow.flyout
    property real radius: Theme.radiusOverlay
    property real blur: depth.blur
    property vector2d offset: Qt.vector2d(0, depth.offset)
    property color color: Qt.rgba(0, 0, 0, depth.opacity)
    readonly property real reach: blur + Math.max(Math.abs(offset.x), Math.abs(offset.y)) + 8

    anchors.fill: target
    anchors.margins: -reach

    component Strip: Item {
        id: strip
        clip: true
        // RectangularShadow is shader-backed. The software scene graph
        // cannot render it; avoid instantiating four unused ShaderEffects
        // (and their Qt-side allocations) in offscreen/CPU rendering.
        Loader {
            active: GraphicsInfo.api !== GraphicsInfo.Unknown
                && GraphicsInfo.api !== GraphicsInfo.Software
                && GraphicsInfo.api !== GraphicsInfo.Null
            sourceComponent: RectangularShadow {
                x: root.reach - strip.x
                y: root.reach - strip.y
                width: root.target.width
                height: root.target.height
                radius: root.radius
                blur: root.blur
                offset: root.offset
                color: root.color
            }
        }
    }

    Strip { x: 0; y: 0; width: root.width; height: root.reach }
    Strip { x: 0; y: root.reach + root.target.height; width: root.width; height: root.reach }
    Strip { x: 0; y: root.reach; width: root.reach; height: root.target.height }
    Strip { x: root.reach + root.target.width; y: root.reach; width: root.reach; height: root.target.height }
}
