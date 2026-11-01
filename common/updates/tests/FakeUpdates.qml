// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton
import QtQuick

// Vela.Updates for the QML tests of Settings and the shell: the tests set
// what it shows, and it records the calls ("install channel=beta").
QtObject {
    id: fake
    property bool available: true
    property string activity: "idle"
    readonly property bool busy: activity !== "idle"
    property string phase: ""
    property string status: "up-to-date"
    property string channel: "experimental"
    property string installedPackage: "vela-git"
    property string installedVersion: "0.1.0.r10.gaaaaaaa-1"
    property string version: "0.1.0"
    property string tag: ""
    property string commit: ""
    readonly property string versionText: channel === "experimental" && commit !== ""
        ? version + " (" + commit.slice(0, 7) + ")" : version
    property var changes: []
    property int changesTotal: 0
    property var notes: []
    property string source: "https://github.com/vela-desktop/vela.git"
    property var lastCheck: new Date()
    property var checkError: ({})
    property string installResult: ""
    property var installError: ({})
    property var installFinished: new Date(NaN)
    property bool needsRelogin: false
    property bool autoCheck: true
    property int checkInterval: 24
    property var nextCheck: new Date(Date.now() + 24 * 3600000)
    property bool waitingForNetwork: false
    property string announce: ""
    property string error: ""
    property var calls: []

    function record(call) { calls = calls.concat([call]) }
    function reset() {
        available = true; activity = "idle"; phase = ""; status = "up-to-date"; channel = "experimental"
        installedPackage = "vela-git"; installedVersion = "0.1.0.r10.gaaaaaaa-1"; version = "0.1.0"; tag = ""
        commit = ""; changes = []; changesTotal = 0; notes = []
        source = "https://github.com/vela-desktop/vela.git"; lastCheck = new Date(); checkError = {}
        installResult = ""; installError = {}; installFinished = new Date(NaN); needsRelogin = false
        autoCheck = true; checkInterval = 24; nextCheck = new Date(Date.now() + 24 * 3600000)
        waitingForNetwork = false; announce = ""; error = ""; calls = []
    }
    // A failure as the client gives it.
    function failure(code, message) {
        return { code: code, message: message || "", reason: reason(code) }
    }
    function reason(code) { return "Reason: " + code }

    function check() { record("check") }
    function install(options) {
        const keys = Object.keys(options || {}).filter(k => options[k] !== "").sort()
        record(["install"].concat(keys.map(k => k + "=" + options[k])).join(" "))
    }
    function setAutoCheck(on) { record("setAutoCheck " + on); autoCheck = on }
    function setCheckInterval(hours) { record("setCheckInterval " + hours); checkInterval = hours }
    function announced(commit) { record("announced " + commit); if (commit === announce) announce = "" }
    function clearError() { error = "" }
}
