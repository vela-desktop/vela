// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQml.Models

// Only the service interfaces consumed by the real Taskbar.qml. No host
// configuration, windows, tray services or compositor socket are touched.
QtObject {
    property QtObject config: QtObject {
        property bool taskView: true
        property int taskbarTrayIcons: 5
        property bool taskbarAllScreens: true
        property bool taskbarNumberHints: true
        property string taskbarClickActive: "previews"
        property string taskbarAlignment: "center"
        property bool endTask: true
        property bool showDesktopCorner: true
        property string startIconSource: ""
        property int startIconRevision: 0
        property bool clockDate: false
        property string clockFormat: "HH:mm"
        property bool notificationCount: true
    }
    property ListModel tasks: ListModel {
        property bool pinsSaved: true
        property var pinnedIds: []
        property var activatedRows: []
        property var launchedRows: []
        property var geometries: ({})
        function activate(row) { activatedRows = activatedRows.concat([row]) }
        function launchNew(row) { launchedRows = launchedRows.concat([row]) }
        function setButtonGeometry(row, window, rect) { geometries[row] = rect }
        function appIds(row) { return ["app-" + row] }
        function isPinned(id) { return false }
        function toggleDesktop() {}
    }
    property ListModel tray: ListModel {
        property int revision: 0
        onDataChanged: ++revision
        readonly property var entries: {
            revision
            const rows = []
            for (let i = 0; i < count; ++i) rows.push(Object.assign({ row: i }, get(i)))
            return rows
        }
        property int activatedRow: -1
        function activate(row, point) { activatedRow = row }
        signal menuReady(int row, real anchorX, var entries)
    }
    property QtObject shell: QtObject {
        property string primaryScreen: ""
        property bool startMenuOpen: false
        signal winXRequested()
        signal showDesktopRequested()
        signal taskbarAppRequested(int index, string action, string output)
        signal superHeld()
        signal superHintsHidden()
        signal superReleased()
        signal notificationCenterRequested()
        signal quickSettingsRequested()
        function now() { return new Date(2026, 10, 15, 23, 59, 59) }
        function placeOnScreen(window, screen) {}
        function placeAtLeft(window, left) {}
        function shiftHeld() { return false }
        function toggleStartMenu() { startMenuOpen = !startMenuOpen }
    }
    property QtObject transfers: QtObject {
        property int activeCount: 0
        property var jobs: []
    }
    property QtObject sharing: QtObject {
        property bool active: false
        property string summary: ""
        property var users: []
    }
    property QtObject audio: QtObject {
        property bool available: true
        property string iconName: ""
        property var microphoneUsers: []
    }
    property QtObject access: QtObject {
        property bool caffeine: false
        property int caffeineSecs: 0
        property int caffeineDurationSecs: 0
    }
    property QtObject status: QtObject {
        property string networkIconName: ""
        property bool batteryPresent: false
        property bool batteryCharging: false
    }
    property QtObject notifications: QtObject {
        property bool automaticDoNotDisturb: false
        property bool doNotDisturb: false
        property bool silenced: false
        property QtObject history: QtObject { property int count: 0 }
        function collectPopups() {}
    }
    property QtObject effects: QtObject { function setBlur(window, rects, radius) {} }
    property QtObject apps: QtObject { function actions(id) { return [] } }
    property QtObject jumps: QtObject {
        function pinned(id) { return [] }
        function recent(id, count) { return [] }
    }
    property QtObject sound: QtObject { function stepTaskVolume(id, delta) { return false } }
}
