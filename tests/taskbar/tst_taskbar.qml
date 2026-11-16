// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Test
import Vela.Taskbar.Test

Item {
    width: 1280
    height: 160

    Component { id: barComponent; Taskbar { visible: true; primary: true } }

    TestCase {
        name: "TaskbarOverflow"
        when: windowShown

        function init() {
            Tasks.clear()
            Tasks.activatedRows = []
            Tasks.launchedRows = []
            Tasks.geometries = ({})
            Tray.clear()
            Tray.activatedRow = -1
            Shell.startMenuOpen = false
            Config.taskView = true
            Config.taskbarAlignment = "center"
            Config.clockDate = false
            Transfers.activeCount = 0
            Transfers.jobs = []
            Audio.microphoneUsers = []
            Notifications.history.count = 0
            Menus.entries = []
            Menus.preview = null
        }

        function addApps(count) {
            for (let i = Tasks.count; i < count; ++i) {
                Tasks.append({ key: "app-" + i, name: "App " + i, iconName: "application-x-executable",
                    windowCount: 1, windowActive: false, desktopId: "app-" + i + ".desktop" })
            }
        }
        function addTray(count) {
            for (let i = Tray.count; i < count; ++i)
                Tray.append({ icon: Qt.resolvedUrl("../../images/vela_splash_169.svg").toString(), title: "Tray " + i, attention: false })
        }
        function bar(width, primary) {
            const b = createTemporaryObject(barComponent, null, { width: width, primary: primary !== false })
            verify(b)
            tryVerify(() => b.visible && b.buttonsAreaWidth > 0)
            tryCompare(b.contentItem, "width", b.width)
            tryCompare(b.contentItem, "height", b.height)
            verify(waitForRendering(b.contentItem))
            return b
        }
        function child(b, name) {
            const item = findChild(b.contentItem, name)
            verify(item !== null, name)
            return item
        }
        function separated(b) {
            const clip = child(b, "taskbarButtonClip")
            const controls = ["taskbarClock", "taskbarTray", "transferIndicator", "systemIcons", "caffeine", "microphone"]
            let left = b.width
            for (const name of controls) {
                const item = child(b, name)
                if (item.visible && item.width > 0)
                    left = Math.min(left, item.mapToItem(b.contentItem, 0, 0).x)
            }
            verify(clip.x >= 0)
            verify(clip.x + clip.width <= left, "app click targets end before all right-side controls: clip="
                + clip.x + "+" + clip.width + " controls=" + left)
        }
        function reachLast(b) {
            const apps = child(b, "taskbarApps")
            const next = child(b, "nextApps")
            const last = child(b, "taskButton-" + (Tasks.count - 1))
            const buttons = child(b, "taskbarButtons")
            tryVerify(() => Math.abs(b.buttonsX - Math.max(b.buttonsLeft,
                Math.min(b.buttonsRight - buttons.width, buttons.preferredX))) < 1)
            for (let i = 0; i < Tasks.count && next.enabled; ++i)
                mouseClick(next)
            tryVerify(() => apps.fullyVisible(last), 3000, "the last app can be reached: x=" + last.x
                + " scroll=" + apps.contentX + " maximum=" + apps.maximumX + " width=" + apps.viewportWidth)
            verify(!next.enabled)
            return last
        }
        function layouts() { return [{ tag: "center" }, { tag: "left", left: true }] }
        function configure(data) { Config.taskbarAlignment = data.left ? "left" : "center" }

        function test_crowded_layout_data() { return layouts() }
        function test_crowded_layout(data) {
            configure(data)
            addApps(24)
            addTray(3)
            Transfers.jobs = [{ finished: false, total: 100, done: 40 }]
            Transfers.activeCount = 1
            const b = bar(900)
            const apps = child(b, "taskbarApps")
            tryVerify(() => apps.overflowing)
            separated(b)
            const last = reachLast(b)
            mouseClick(last)
            compare(Tasks.activatedRows[0], 23)
            mouseClick(last, last.width / 2, last.height / 2, Qt.MiddleButton)
            compare(Tasks.launchedRows[0], 23)
            mouseClick(last, last.width / 2, last.height / 2, Qt.RightButton)
            verify(Menus.entries.some(e => e.text === "Close window"), "hidden apps keep their jump lists")
            const previous = child(b, "previousApps")
            while (previous.enabled)
                mouseClick(previous)
            compare(apps.contentX, 0)
            separated(b)
        }

        function test_changes_in_clock_tray_and_indicators_data() { return layouts() }
        function test_changes_in_clock_tray_and_indicators(data) {
            configure(data)
            addApps(20)
            const b = bar(1000)
            const apps = child(b, "taskbarApps")
            tryVerify(() => apps.width > 0)
            const initialWidth = apps.width
            reachLast(b)
            addTray(4)
            Transfers.activeCount = 1
            Transfers.jobs = [{ finished: false, total: 100, done: 40 }]
            Audio.microphoneUsers = [{ name: "Recorder" }]
            Notifications.history.count = 3
            tryVerify(() => apps.width < initialWidth)
            separated(b)
            reachLast(b)
            Config.taskView = false
            b.width = 1400
            Tray.clear()
            Transfers.activeCount = 0
            Audio.microphoneUsers = []
            tryVerify(() => !apps.overflowing)
            tryCompare(apps, "contentX", 0)
            verify(!child(b, "nextApps").visible)
            verify(!child(b, "previousApps").visible)
            separated(b)
        }

        function test_active_app_is_revealed_and_minimize_targets_stay_in_the_strip() {
            addApps(30)
            const b = bar(800)
            const apps = child(b, "taskbarApps")
            tryVerify(() => apps.overflowing)
            Tasks.setProperty(29, "windowActive", true)
            const last = child(b, "taskButton-29")
            tryVerify(() => apps.fullyVisible(last))
            tryVerify(() => Tasks.geometries[29] !== undefined)
            verify(Tasks.geometries[29].width > 0)
            for (const rect of Object.values(Tasks.geometries)) {
                verify(rect.width > 0)
                verify(rect.x >= b.buttonsLeft)
                verify(rect.x + rect.width <= b.buttonsRight)
            }
            Tasks.setProperty(29, "windowActive", false)
            Tasks.setProperty(0, "windowActive", true)
            tryCompare(apps, "contentX", 0)
        }

        function test_navigation_has_keyboard_and_accessible_names() {
            addApps(30)
            const b = bar(800)
            const apps = child(b, "taskbarApps")
            const next = child(b, "nextApps")
            tryVerify(() => next.enabled)
            const state = AccessibleProbe.state(next)
            compare(state.role, Accessible.Button)
            compare(state.name, "Next apps")
            const before = apps.contentX
            next.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            verify(apps.contentX > before)
            const previous = child(b, "previousApps")
            compare(AccessibleProbe.state(previous).name, "Previous apps")
            previous.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Space)
            compare(apps.contentX, 0)
        }

        function test_uncrowded_bar_keeps_its_alignment_and_start_does_not_scroll() {
            addApps(3)
            const b = bar(1280)
            const buttons = child(b, "taskbarButtons")
            const apps = child(b, "taskbarApps")
            tryVerify(() => buttons.width > 0)
            tryVerify(() => Math.abs(b.buttonsX - Math.round((b.width - buttons.width) / 2)) < 1)
            verify(!apps.overflowing)
            addApps(30)
            tryVerify(() => apps.overflowing)
            const start = child(b, "startButton")
            tryVerify(() => b.buttonsX <= b.buttonsRight - buttons.width + 1)
            const x = start.mapToItem(b.contentItem, 0, 0).x
            reachLast(b)
            compare(start.mapToItem(b.contentItem, 0, 0).x, x)
            mouseClick(start)
            verify(Shell.startMenuOpen)
        }

        function test_oversized_tray_scrolls_without_taking_the_last_app_slot() {
            addApps(24)
            addTray(40)
            const b = bar(800)
            const apps = child(b, "taskbarApps")
            const tray = child(b, "taskbarTray")
            tryVerify(() => tray.overflowing)
            verify(apps.viewportWidth >= 44, "tray leaves room for at least one app")
            separated(b)
            reachLast(b)
            const next = findChild(tray, "nextApps")
            for (let i = 0; i < Tray.count && next.enabled; ++i)
                mouseClick(next)
            compare(tray.contentX, tray.maximumX)
            // The last icon is fully visible at the end of the tray viewport.
            const viewport = findChild(tray, "appViewport")
            mouseClick(viewport, viewport.width - 16, viewport.height / 2)
            compare(Tray.activatedRow, 39)
            separated(b)
        }

        function test_removing_apps_clamps_scroll_and_restores_the_unclipped_row() {
            addApps(30)
            const b = bar(800)
            const apps = child(b, "taskbarApps")
            reachLast(b)
            Tasks.remove(2, 28)
            tryVerify(() => !apps.overflowing)
            tryCompare(apps, "contentX", 0)
            verify(apps.fullyVisible(child(b, "taskButton-1")))
        }

        function test_secondary_bar_only_reserves_its_clock() {
            addApps(30)
            const b = bar(800, false)
            const apps = child(b, "taskbarApps")
            tryVerify(() => apps.width > 0)
            const initialWidth = apps.width
            addTray(6)
            Transfers.activeCount = 1
            Audio.microphoneUsers = [{ name: "Recorder" }]
            verify(waitForRendering(b.contentItem))
            compare(apps.width, initialWidth)
            b.width = 700
            tryVerify(() => apps.width < initialWidth)
            separated(b)
            reachLast(b)
        }
    }
}
