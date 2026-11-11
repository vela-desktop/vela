// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Effects

// What floats casts a soft shadow, drawn only *outside* it: acrylic panels are
// semi-transparent and must show the blurred background below, not a shadow.
// RectangularShadow (analytic, like the compositor's) clipped into four strips
// around the target. `depth` is one of Theme.shadow's (flyout, dialog); the
// window must leave `reach` around the target, or the shadow is cut.
// `above` and `below` cut it closer above and below the target, where it
// would fall on another translucent panel (the Notification Center's two).
Item {
    id: root

    required property Item target
    property var depth: Theme.shadow.flyout
    property real radius: Theme.radiusOverlay
    property real blur: depth.blur
    property vector2d offset: Qt.vector2d(0, depth.offset)
    property color color: Qt.rgba(0, 0, 0, depth.opacity)
    readonly property real reach: blur + Math.max(Math.abs(offset.x), Math.abs(offset.y)) + 8
    property real above: reach
    property real below: reach

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
            sourceComponent: Component {
                RectangularShadow {
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
    }

    Strip { x: 0; y: root.reach - height; width: root.width; height: Math.min(root.above, root.reach) }
    Strip { x: 0; y: root.reach + root.target.height; width: root.width; height: Math.min(root.below, root.reach) }
    Strip { x: 0; y: root.reach; width: root.reach; height: root.target.height }
    Strip { x: root.reach + root.target.width; y: root.reach; width: root.reach; height: root.target.height }
    // Expressive: the corners too, inside the target's box, where its round
    // corners leave the strips' gap (a lighter square at M3's 28). Its
    // tonal surfaces are opaque and every Shadow lies beneath its target, so
    // only the part outside the round corner shows.
    Repeater {
        model: Theme.expressive && root.radius > 0 ? 4 : 0
        delegate: Strip {
            required property int index
            readonly property real size: Math.min(root.radius, root.target.width / 2, root.target.height / 2)
            x: index % 2 === 0 ? root.reach : root.reach + root.target.width - size
            y: index < 2 ? root.reach : root.reach + root.target.height - size
            width: size
            height: size
        }
    }
}
