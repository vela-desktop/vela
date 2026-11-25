// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The notification center's small buttons: subtle, or the accent while on;
// `tonal`, the expressive variant's filled tonal button ("Clear all").
// `toggle`: a button that is on or off (Do not disturb), its state `on` kept
// by its owner. A screen reader hears Qt's own check state (Qt's button
// writes it over the attached Accessible one once accessibility is on), so
// a toggle is checkable, follows `on` and never toggles itself: Qt's toggle,
// before `clicked`, is put back to `on`, which the owner then changes.
Button {
    id: button
    property bool tonal: false
    property bool toggle: false
    property bool on: false
    subtle: !accent && !tonal
    checkable: toggle
    checked: toggle && on
    onToggled: checked = Qt.binding(() => button.toggle && button.on)
    Accessible.role: Accessible.Button
    implicitWidth: text === "" ? 32 : implicitContentWidth + 20
    implicitHeight: 28
    leftPadding: 10
    rightPadding: 10
    font.pixelSize: Theme.fontCaption
}
