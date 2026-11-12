// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The layer under the command bar that holds the navigation pane, the
// files and the status bar (TabPage.qml), on Theme.layer with the overlay's
// corners on top and its outline.
// Expressive: on surface container low (Theme.layer) with large top
// corners and no outline.
Rectangle {
    objectName: "filesLayer"
    color: Theme.layer
    topLeftRadius: Theme.expressive ? Theme.shapeLarge : Theme.radiusOverlay
    topRightRadius: Theme.expressive ? Theme.shapeLarge : Theme.radiusOverlay
    border.width: Theme.expressive ? 0 : 1
    border.color: Theme.layerStroke
}
