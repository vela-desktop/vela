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
    Component { id: overflowComponent; TrayOverflow {} }

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
            Config.clockFormat = "HH:mm"
            Transfers.activeCount = 0
            Transfers.jobs = []
            Sharing.active = false
            Audio.microphoneUsers = []
            Notifications.history.count = 0
            Styles.taskbar.floating = false
            Styles.taskbar.fit = false
            Styles.taskbar.edge = 0
            Styles.taskbar.margin = 0
            Styles.taskbar.sideMargin = 0
            Styles.taskbar.splitGap = 0
            Styles.taskbar.height = 48
            Menus.entries = []
            Menus.preview = null
            Menus.trayOverflow = null
            Config.taskbarTrayIcons = 5
            Config.taskbarClickActive = "previews"
        }

        function addApps(count) {
            for (let i = Tasks.count; i < count; ++i) {
                Tasks.append({ key: "app-" + i, name: "App " + i, iconName: "application-x-executable",
                    windowIds: [], attention: false, activeWindowId: "", badgeCount: "", progress: -1, windowCount: 1, windowActive: false, pinned: false, hasWindows: true, desktopId: "app-" + i + ".desktop" })
            }
        }
        function addTray(count) {
            for (let i = Tray.count; i < count; ++i)
                Tray.append({ icon: Qt.resolvedUrl("../../images/vela_splash_169.svg").toString(), title: "Tray " + i, attention: false, passive: false })
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
            const controls = ["taskbarClock", "taskbarTray", "transferIndicator", "screenSharing", "systemIcons", "caffeine", "microphone"]
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
        function layouts() {
            return [
                { tag: "center" }, { tag: "left", left: true },
                { tag: "floating-center", floating: true },
                { tag: "floating-left", floating: true, left: true },
                { tag: "fitted", floating: true, fit: true },
                { tag: "split", floating: true, fit: true, split: 8 },
                { tag: "edge-center", floating: true, fit: true, edge: 1 },
                { tag: "edge-left", floating: true, fit: true, edge: 2 }
            ]
        }
        function configure(data) {
            Config.taskbarAlignment = data.left ? "left" : "center"
            Styles.taskbar.floating = !!data.floating
            Styles.taskbar.fit = !!data.fit
            Styles.taskbar.edge = data.edge || 0
            Styles.taskbar.splitGap = data.split || 0
            Styles.taskbar.margin = data.floating ? 10 : 0
            Styles.taskbar.sideMargin = data.floating ? 100 : 0
            Styles.taskbar.height = data.floating ? 68 : 48
        }

        function test_crowded_layout_data() { return layouts() }
        function test_crowded_layout(data) {
            configure(data)
            Config.clockFormat = "h:mm:ss AP"
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
            Config.clockFormat = "h:mm:ss AP"
            addTray(4)
            Transfers.activeCount = 1
            Transfers.jobs = [{ finished: false, total: 100, done: 40 }]
            Sharing.active = true
            Audio.microphoneUsers = [{ name: "Recorder" }]
            Notifications.history.count = 3
            tryVerify(() => apps.width < initialWidth)
            separated(b)
            reachLast(b)
            Config.taskView = false
            b.width = 1400
            Tray.clear()
            Transfers.activeCount = 0
            Sharing.active = false
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

        function test_multiwindow_click_and_shortcuts() {
            addApps(16)
            Tasks.setProperty(0, "windowCount", 2)
            Tasks.setProperty(0, "windowActive", true)
            const b = bar(800)
            const first = child(b, "taskButton-0")
            mouseClick(first)
            verify(Menus.preview.latched)
            compare(Menus.preview.button, first)
            mouseClick(first)
            compare(Menus.preview, null)
            compare(Tasks.activatedRows.length, 0)
            Config.taskbarClickActive = "next"
            mouseClick(first)
            compare(Tasks.activatedRows, [0])

            Shell.primaryScreen = b.screenName
            Shell.superHeld()
            compare(first.shortcutNumber, "1")
            compare(child(b, "taskButton-9").shortcutNumber, "0")
            compare(child(b, "taskButton-10").shortcutNumber, "")
            Shell.superHintsHidden()
            compare(first.shortcutNumber, "")
            Shell.taskbarAppRequested(9, "new", b.screenName)
            compare(Tasks.launchedRows, [9])
        }

        function test_attention_and_badge_roles() {
            addApps(1)
            Tasks.setProperty(0, "attention", true)
            Tasks.setProperty(0, "badgeCount", "99+")
            Tasks.setProperty(0, "progress", 0.4)
            Notifications.silenced = true
            const b = bar(1000)
            const task = child(b, "taskButton-0")
            verify(task.needsAttention)
            verify(task.attentionLit)
            verify(!task.flashAttention)
            compare(task.countBadge, "99+")
            compare(task.progressValue, 0.4)
            Tasks.setProperty(0, "attention", false)
            tryCompare(task, "needsAttention", false)
            Notifications.silenced = false
        }

        function test_oversized_tray_keeps_room_for_an_app() {
            addApps(24)
            addTray(40)
            Config.taskbarTrayIcons = -1
            const b = bar(800)
            const apps = child(b, "taskbarApps")
            verify(apps.viewportWidth >= Styles.taskbar.buttonWidth)
            verify(b.hiddenTrayIcons.length > 0)
            separated(b)
            reachLast(b)
            mouseClick(child(b, "trayOverflowButton"))
            compare(Menus.trayOverflow.bar, b)
        }

        function test_hidden_tray_icon_activates_before_the_panel_closes() {
            addTray(1)
            Config.taskbarTrayIcons = 0
            const b = bar(1000)
            const overflow = createTemporaryObject(overflowComponent, null)
            mouseClick(child(b, "trayOverflowButton"))
            tryVerify(() => overflow.visible)
            mouseClick(child(overflow, "trayOverflowIcon-0"))
            compare(Tray.activatedRow, 0)
            tryCompare(overflow, "visible", false)
        }

        function test_tray_status_and_limit() {
            addTray(7)
            Tray.setProperty(1, "passive", true)
            Tray.setProperty(6, "attention", true)
            Config.taskbarTrayIcons = 2
            const b = bar(1000)
            compare(b.trayIcons.shown.map(item => item.row), [6, 0, 2])
            compare(b.hiddenTrayIcons.map(item => item.row), [1, 3, 4, 5])
            Config.taskbarTrayIcons = 0
            tryCompare(b.hiddenTrayIcons, "length", 6)
            compare(b.trayIcons.shown.map(item => item.row), [6])
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
            Sharing.active = true
            Audio.microphoneUsers = [{ name: "Recorder" }]
            compare(apps.width, initialWidth)
            Config.clockFormat = "h:mm:ss AP"
            tryVerify(() => apps.width < initialWidth)
            separated(b)
            reachLast(b)
        }
    }
}
