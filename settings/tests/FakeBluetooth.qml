// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton
import QtQuick

// Vela.Bluetooth for Settings' QML tests: the tests set what it shows, and
// it records the calls ("add /org/bluez/hci0/dev_1").
QtObject {
    id: fake
    property bool available: true
    property bool platformBlocked: false
    property bool active: true
    property bool powered: true
    property var adapter: ({ name: "vela-pc", address: "AA:AA:AA:AA:AA:00", powered: true, discoverable: false, discovering: false })
    property var adapters: [{ path: "/org/bluez/hci0", name: "vela-pc", address: "AA:AA:AA:AA:AA:00", inUse: true }]
    property var devices: []
    readonly property var paired: devices.filter(d => d.paired)
    readonly property var found: devices.filter(d => !d.paired)
    property var question: ({})
    property string pairing: ""
    property bool searching: false
    property bool searchEnded: false
    property string error: ""
    property var calls: []

    signal added(string path)
    signal questionClosed(string id, string reason)

    function record(call) { calls = calls.concat([call]) }
    function reset() {
        available = true; platformBlocked = false; active = true; powered = true
        adapter = { name: "vela-pc", address: "AA:AA:AA:AA:AA:00", powered: true, discoverable: false, discovering: false }
        devices = []; question = {}; pairing = ""; searching = false; searchEnded = false; error = ""; calls = []
    }
    // A device entry, as the model gives it.
    function device(n, changes) {
        return Object.assign({ path: "/org/bluez/hci0/dev_" + n, address: "00:11:22:33:44:0" + n, name: "Device " + n,
                               icon: "input-mouse", type: "mouse", paired: false, trusted: false, connected: false,
                               blocked: false, battery: -1, rssi: 0, operation: "", error: "", message: "", waiting: false },
                             changes || {})
    }

    function setPowered(on) { record("setPowered " + on) }
    function useAdapter(address) { record("useAdapter " + address) }
    function add(path) { record("add " + path) }
    function connectDevice(path) { record("connect " + path) }
    function disconnectDevice(path) { record("disconnect " + path) }
    function removeDevice(path) { record("remove " + path) }
    function rename(path, name) { record("rename " + path + " " + name) }
    function cancel(path) { record("cancel " + path) }
    function answer(id, response, value) { record("answer " + id + " " + response + (value ? " " + value : "")) }
    function startSearch() { record("startSearch") }
    function stopSearch() { record("stopSearch") }
    function stopBeingVisible() { record("stopBeingVisible") }
    function clearError() { error = "" }
}
