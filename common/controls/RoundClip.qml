// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Effects

// What it holds, cut to round corners: a picture or a thumbnail inside a
// round shape, whose own corners are square (docs/visual-coherence.md
// §6.12: `radius` is the outer shape's less the gap, Theme.nested). Qt Quick
// clips to rectangles only, so the content is drawn to a layer and masked;
// with `radius` 0, and where there are no shaders (the software renderer of
// the tests), it is a plain clip and costs nothing.
Item {
    id: clipper

    property real radius: 0
    default property alias content: holder.data
    readonly property bool masked: radius > 0
        && GraphicsInfo.api !== GraphicsInfo.Unknown
        && GraphicsInfo.api !== GraphicsInfo.Software
        && GraphicsInfo.api !== GraphicsInfo.Null

    Item {
        id: holder
        anchors.fill: parent
        clip: true
        layer.enabled: clipper.masked
        layer.smooth: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: mask
            // A hard edge one pixel soft: the mask's own antialiasing.
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
    }
    Item {
        id: mask
        anchors.fill: parent
        visible: false
        layer.enabled: clipper.masked
        layer.smooth: true
        Rectangle {
            objectName: "roundClipMask"
            anchors.fill: parent
            radius: Math.min(clipper.radius, width / 2, height / 2)
        }
    }
}
