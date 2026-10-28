// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

.pragma library

// A Bluetooth device as Settings shows it (docs/bluetooth.md §9.1), from
// Vela.Bluetooth's entry: what it is doing, or how its last operation
// ended, or its state.
function state(d) {
    if (d.waiting)
        return qsTr("Waiting for you...")
    switch (d.operation) {
    case "pairing":
    case "trusting":
        return qsTr("Pairing...")
    case "connecting":
        return qsTr("Connecting...")
    case "disconnecting":
        return qsTr("Disconnecting...")
    case "removing":
        return qsTr("Removing...")
    case "renaming":
        return qsTr("Renaming...")
    }
    if (d.message)
        return d.message
    if (!d.paired)
        return ""
    return d.connected ? qsTr("Connected") : qsTr("Paired")
}

// The state, and the battery when the device reports it.
function description(d) {
    const parts = [state(d)]
    if (d.battery >= 0)
        parts.push(qsTr("Battery %1%").arg(d.battery))
    return parts.filter(part => part !== "").join(" · ")
}

// BlueZ's icons (input-mouse, audio-headset...) are theme names; the
// daemon says "bluetooth" when BlueZ has none.
function icon(d) {
    return d.icon && d.icon !== "bluetooth" ? d.icon : "preferences-system-bluetooth"
}

// The steps the user can stop (docs/bluetooth.md §6.3, Cancel).
function cancellable(d) {
    return d.operation === "pairing" || d.operation === "trusting" || d.operation === "connecting"
}
