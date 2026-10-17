// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// A number animated on the desktop's one curve, decelerate, in `normal`
// unless told otherwise: entrances, exits and state changes alike
// (docs/visual-coherence.md §6.4).
NumberAnimation {
    duration: Theme.normal
    easing.type: Easing.BezierSpline
    easing.bezierCurve: Theme.decelerate
}
