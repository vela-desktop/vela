// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// A control's background: one color, chosen by the control from its state
// combination (pressed over hovered over rest; selected or checked choose the
// family, as WinUI's brushes do), and every change of it fades with the
// desktop's timing (docs/visual-coherence.md §6.8). A hover that leaves
// halfway turns back from the current color. `animated: false` while a
// recycled delegate takes a new item, so it takes its state without a fade.
Rectangle {
    property bool animated: true
    radius: Theme.radius
    Behavior on color {
        enabled: animated
        ColorAnimation { duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }
}
