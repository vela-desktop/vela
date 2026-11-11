// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// A position, a size or a corner animated on M3 Expressive's spatial curve of
// a speed ("fast", "default", "slow") under motion-variant=expressive, so it
// overshoots a little and settles; with the default variant, Eased's curve in
// `rest`, so the default look moves as before. Never for colors or opacity
// (they take ColorFade and Eased), nor for exits (they don't bounce).
NumberAnimation {
    property string speed: "default"
    property int rest: Theme.normal
    duration: Theme.spatialDuration(speed, rest)
    easing.type: Easing.BezierSpline
    easing.bezierCurve: Theme.spatialCurve(speed)
}
