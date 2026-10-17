// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The room a panel's window leaves around it for its shadow
// (docs/visual-coherence.md §6.9): a press there is a press outside the
// panel, `outside()`; on the panel's own background it goes on to what is
// below. Under the panel's PanelMotion, filling the window, the way
// ContextMenu.qml handles its whole window. An input region would let the
// press through to the windows below, but needs a path from QML to
// QWindow::setMask().
MouseArea {
    // The items that are the panel (a PanelMotion can hold more than one,
    // as the Notification Center's two).
    required property list<Item> panels
    signal outside()

    anchors.fill: parent
    onPressed: mouse => {
        for (const panel of panels) {
            if (panel.contains(mapToItem(panel, mouse.x, mouse.y))) {
                mouse.accepted = false
                return
            }
        }
        outside()
    }
}
