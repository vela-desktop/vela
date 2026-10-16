// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// A color faded on the desktop's one curve, decelerate, in `fast`: what a
// state change does to a fill or a border (docs/visual-coherence.md §6.8).
// Eased's twin for colors.
ColorAnimation {
    duration: Theme.fast
    easing.type: Easing.BezierSpline
    easing.bezierCurve: Theme.decelerate
}
