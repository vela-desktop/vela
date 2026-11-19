// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The taskbar: Start button, pinned and open apps in the center, clock on the
// right.
Window {
    id: root
    objectName: "taskbar"
    // One per output (ScreenWindows): the main one also has the notification
    // area and system icons, the others the clock, like Windows.
    property bool primary: false
    property var taskModel: Tasks
    property bool showNumberHints: false
    readonly property string screenName: screen ? screen.name : ""

    // It stays invisible until main.cpp has turned it into a layer-shell
    // panel. The compositor decides its length (anchored to both ends of its
    // edge); its thickness is the bar's.
    visible: false
    width: vertical ? Styles.taskbar.thickness : 1280
    height: vertical ? 800 : Styles.taskbar.height
    color: "transparent"

    // The edge it sits on (docs/taskbar.md §11): a row at the bottom or the
    // top, a column on the left or the right.
    readonly property string position: style.position
    readonly property bool vertical: style.vertical
    readonly property bool atTop: position === "top"
    // An item's place along the bar and its size there, in the bar's
    // coordinates.
    function along(item) {
        const p = item.mapToItem(root.contentItem, 0, 0)
        return vertical ? p.y : p.x
    }
    function lengthOf(item) {
        return vertical ? item.height : item.width
    }

    // A taskbar style's layout (Styles.taskbar): floating, it is a pill (or
    // two) inside the window; the window keeps the bar's height, so windows
    // and panels stay where they are.
    readonly property QtObject style: Styles.taskbar
    readonly property bool floating: style.floating
    // fit: apps and tray in one pill that shrinks to them; edge: the tray in
    // a pill of its own at the screen's right edge.
    // A pill that fits its apps, a split tray or the tray at the screen's edge
    // are row layouts: a vertical bar is one pill along the edge (§11.2).
    readonly property bool fit: floating && style.fit && !vertical
    readonly property bool edge: fit && style.edge > 0
    readonly property bool split: fit && !edge && style.splitGap > 0
    readonly property real fitGap: 8 + style.splitGap
    // Inside a fitted pill, the room before the Start button and after the
    // clock: a round end (radius up to half the height) would cut what sits
    // in it, the clock's highlight and the notifications' badge.
    readonly property int fitPadding: 10
    readonly property int edgeGap: Math.max(style.margin, 8) // the tray pill's gap to the screen's edge
    // A floating pill sits this far inside the bar above and below: the tray's
    // and the clock's highlights keep inside it, as tall as on Vela's bar.
    readonly property int inset: floating ? style.margin : 0
    // The notification area, the system icons and the clock, side by side.
    readonly property real fixedTrayWidth: (transferIndicator.visible ? transferIndicator.width + 4 : 0)
        + screenSharing.width + microphone.width + caffeine.width
        + (systemIcons.visible ? systemIcons.width + 2 : 0) + clock.width
    readonly property real trayGroupWidth: fixedTrayWidth + (tray.visible ? tray.width + 4 : 0)
    // A floating bar that spans the screen: its side margin, shrunk when the
    // screen is too narrow for it (the tray must still fit beside the apps).
    readonly property int sideMargin: Math.min(style.sideMargin, Math.max(style.margin,
        Math.floor((width - naturalButtonsWidth) / 2 - (trayGroupWidth + 20))))

    readonly property real fixedButtonsWidth: style.buttonWidth * (Config.taskView ? 2 : 1)
        + (Config.taskView ? style.spacing : 0)
    readonly property real naturalButtonsWidth: fixedButtonsWidth
        + (taskRepeater.count > 0 ? style.spacing + appStrip.implicitWidth : 0)
    readonly property real minimumLeft: fit ? style.margin + fitPadding : 12 + (floating ? style.margin : 0)
    readonly property real minimumRightInset: edge ? edgeGap + 12 : fit ? style.margin + fitPadding
        : 12 + (floating ? style.margin : 0)
    readonly property real groupGap: edge ? 28 : fit ? fitGap : 8
    // Reserve room for app paging before choosing how many tray icons fit.
    // Minimum margins keep this independent of the actual tray width.
    readonly property real trayMaximumWidth: Math.max(0, width - minimumLeft - minimumRightInset - groupGap
        - fixedButtonsWidth - (taskRepeater.count > 0 ? style.spacing + appStrip.implicitWidth : 0) - fixedTrayWidth - 4)
    readonly property real buttonsLeft: fit ? minimumLeft : 12 + (floating ? sideMargin : 0)
    readonly property real buttonsRight: width - (fit ? minimumRightInset : 12 + (floating ? sideMargin : 0))
        - trayGroupWidth - groupGap
    readonly property real buttonsAreaWidth: Math.max(0, buttonsRight - buttonsLeft)
    readonly property real buttonsX: buttonClip.x + buttons.x

    readonly property int traySlotWidth: 32

    // --- a vertical bar: the same pieces in a column, Start at the top, the
    // tray and the clock at the bottom (§11.2) ---
    readonly property real columnStart: 12 + (floating ? style.margin : 0)
    readonly property real columnEnd: 8 + (floating ? style.margin : 0)
    readonly property real fixedButtonsHeight: style.buttonHeight * (Config.taskView ? 2 : 1)
        + (Config.taskView ? style.spacing : 0)
    readonly property real fixedTrayHeight: (transferIndicator.visible ? transferIndicator.height + 4 : 0)
        + screenSharing.height + microphone.height + caffeine.height
        + (systemIcons.visible ? systemIcons.height + 2 : 0) + clock.height + (showDesktop.visible ? showDesktop.height : 0)
    readonly property real trayGroupHeight: fixedTrayHeight + (tray.visible ? tray.height + 4 : 0)
    readonly property real buttonsTop: columnStart
    readonly property real buttonsBottom: height - columnEnd - trayGroupHeight - 8
    // What the tray may take, after Start, Task View and room for one app.
    readonly property real trayMaximumHeight: Math.max(0, height - columnStart - columnEnd - 8 - fixedButtonsHeight
        - (taskRepeater.count > 0 ? style.spacing + appStrip.implicitHeight : 0) - fixedTrayHeight - 4)
    readonly property real trayEssentialHeight: Math.max(0, height - columnStart - columnEnd - 8 - fixedButtonsHeight
        - (taskRepeater.count > 0 ? style.spacing + appStrip.minimumHeight : 0) - fixedTrayHeight - 4)
    readonly property real trayEssentialWidth: Math.max(0, width - minimumLeft - minimumRightInset - groupGap
        - fixedButtonsWidth - (taskRepeater.count > 0 ? style.spacing + appStrip.minimumWidth : 0) - fixedTrayWidth - 4)
    // Each icon is on the taskbar or in the drawer, as the user chose
    // (Config.taskbarTrayShown, docs/taskbar.md §8): the chosen ones in their
    // order, then those asking for attention; a passive one waits in the
    // drawer. Short of room, the last chosen ones go in the drawer first.
    property string draggedTrayKey: "" // an icon being dragged: "^" shows
    readonly property var trayIcons: {
        const entries = root.primary ? Tray.entries : []
        const picked = []
        for (const key of Config.taskbarTrayShown) {
            const item = entries.find(entry => entry.key === key)
            if (item && !item.passive && picked.indexOf(item) < 0) picked.push(item)
        }
        const urgent = entries.filter(item => item.attention && picked.indexOf(item) < 0)
        const capacity = Math.floor((root.vertical ? root.trayMaximumHeight : root.trayMaximumWidth) / root.traySlotWidth)
        const essential = Math.floor((root.vertical ? root.trayEssentialHeight : root.trayEssentialWidth) / root.traySlotWidth)
        // Not on draggedTrayKey: a new list would recreate the icon being
        // dragged, and lose the drag.
        const drawer = entries.length > picked.length + urgent.length || picked.length + urgent.length > capacity
        const reserved = drawer ? 1 : 0
        const shownUrgent = Math.min(Math.max(0, essential - reserved), urgent.length)
        const shownPicked = Math.min(picked.length, Math.max(0, capacity - reserved - shownUrgent))
        const shown = picked.slice(0, shownPicked).concat(urgent.slice(0, shownUrgent))
        return { shown: shown, hidden: entries.filter(item => !shown.some(icon => icon.row === item.row)) }
    }
    readonly property var hiddenTrayIcons: trayIcons.hidden

    function trayKeyShown(key) {
        return Config.taskbarTrayShown.indexOf(key) >= 0
    }
    // Puts an icon on the taskbar before the shown icon `before` (by key; ""
    // after the last), or in the drawer with `shown` false. The keys of apps
    // not running now keep their place in the list.
    function placeTrayIcon(key, shown, before) {
        if (key === "") return
        const list = Config.taskbarTrayShown.filter(k => k !== key)
        if (shown) {
            let at = before ? list.indexOf(before) : -1
            if (at < 0) {
                const last = root.trayIcons.shown.filter(item => item.key !== key && list.indexOf(item.key) >= 0).pop()
                at = last ? list.indexOf(last.key) + 1 : list.length
            }
            list.splice(at, 0, key)
        }
        Config.setTaskbarTrayShown(list)
    }
    // Where a dragged icon would land, from a point in the taskbar's
    // coordinates: "drawer", or { before: key } among the shown icons. "^"
    // is the drawer only for an icon coming from the taskbar: one coming
    // from the drawer lands wherever it is let go on the bar.
    function trayDropTarget(x, y, fromDrawer) {
        if (offBar(x, y)) return "drawer" // dragged off the bar, towards the drawer
        const over = overflowButton.mapFromItem(root.contentItem, x, y)
        if (!fromDrawer && overflowButton.visible && overflowButton.contains(over)) return "drawer"
        const position = vertical ? y : x
        let before = ""
        let marker = -1
        for (let i = 0; i < trayRepeater.count; ++i) {
            const icon = trayRepeater.itemAt(i)
            if (!icon || icon.entry.key === root.draggedTrayKey) continue
            const start = along(icon)
            if (position < start + lengthOf(icon) / 2) {
                before = icon.entry.key
                marker = start
                break
            }
            marker = start + lengthOf(icon)
        }
        return { before: before, marker: marker }
    }
    // A point in the bar's coordinates past its inner edge, towards the
    // middle of the screen (and the drawer); and one on the bar itself.
    function offBar(x, y) {
        return position === "top" ? y >= height : position === "left" ? x >= width
            : position === "right" ? x < 0 : y < 0
    }
    function onBar(x, y) {
        return x >= 0 && x < width && y >= 0 && y < height
    }
    property real trayMarkerX: -1
    function trayDragMoved(key, x, y, fromDrawer) {
        root.draggedTrayKey = key
        const target = trayDropTarget(x, y, fromDrawer)
        root.trayMarkerX = target === "drawer" || target.marker < 0 ? -1 : target.marker
    }
    function trayDropped(key, x, y, fromDrawer) {
        const target = trayDropTarget(x, y, fromDrawer)
        trayDragCanceled()
        // Later: the dragged icon's own delegate may go away with the change.
        if (target === "drawer") Qt.callLater(placeTrayIcon, key, false)
        else if (onBar(x, y)) Qt.callLater(placeTrayIcon, key, true, target.before)
    }
    function trayDragCanceled() {
        root.draggedTrayKey = ""
        root.trayMarkerX = -1
    }

    // The apps pinned at first start (.desktop ids; those not installed are
    // skipped). Afterwards the user's choices apply: pinned and unpinned from
    // the menus, reordered by dragging the buttons.
    readonly property list<string> pinnedIds: [
        "vela-files.desktop",
        "org.kde.konsole.desktop",
        "firefox.desktop",
        "org.mozilla.firefox.desktop",
        "chromium.desktop",
        "org.kde.kate.desktop",
        "vela-settings.desktop"
    ]
    // Only at first start: afterwards the saved ones apply (pinned and
    // unpinned from the menus).
    Component.onCompleted: {
        if (!Tasks.pinsSaved) {
            Tasks.pinnedIds = pinnedIds
        }
        updateBlur()
        Menus.setTrayAnchors(root.screenName, trayAnchors)
    }

    // The whole taskbar above the blurred background (acrylic). A single
    // floating pill: only it, rounded. Two pills: the whole window, the
    // panel's alpha gives their shape.
    function updateBlur() {
        if (floating && !split && !edge) {
            Effects.setBlur(root, [Qt.rect(floatingSurface.x, floatingSurface.y, floatingSurface.width, floatingSurface.height)],
                style.radius)
        } else {
            Effects.setBlur(root, [Qt.rect(0, 0, width, height)])
        }
    }
    onWidthChanged: updateBlur()
    onHeightChanged: updateBlur()

    // A button's window previews (TaskbarPreview.qml).
    Timer {
        id: previewDelay
        property var task: null
        interval: 450
        onTriggered: {
            if (!task || !task.hovered || !appStrip.fullyVisible(task)) return
            if (task.windowCount > 0) {
                root.openPreviews(task, false)
            } else {
                Menus.tooltip = { owner: task, text: task.tooltip, center: root.centerOnScreen(task), screen: root.screenName }
            }
        }
    }

    function openPreviews(task, latched, cycling, spring) {
        Menus.preview = { button: task, center: root.centerOnScreen(task), screen: root.screenName, latched: latched,
                          cycling: !!cycling, startingWindow: cycling ? task.activeWindowId : "", spring: !!spring }
    }

    function activateButton(task) {
        previewDelay.stop()
        if (Menus.tooltip && Menus.tooltip.owner === task) Menus.tooltip = null
        if (Menus.preview && Menus.preview.latched && Menus.preview.button === task) {
            Menus.preview = null
        } else if (task.windowActive && task.windowCount > 1 && !task.attention
                   && Config.taskbarClickActive !== "next") {
            openPreviews(task, true)
        } else {
            Menus.preview = null
            root.taskModel.activate(task.index)
        }
    }

    Connections {
        target: Shell
        function onSuperHeld() { root.showNumberHints = true }
        function onSuperHintsHidden() { root.showNumberHints = false }
        function onSuperReleased() { root.showNumberHints = false }
        function onTaskbarAppRequested(index, action, output) {
            const target = Config.taskbarAllScreens && Qt.application.screens.some(s => s.name === output)
                ? output : Shell.primaryScreen
            if (root.screenName !== target) return
            const task = taskRepeater.itemAt(index)
            if (!task) return
            previewDelay.stop()
            appStrip.reveal(task)
            if (action === "new") {
                Menus.preview = null
                root.taskModel.launchNew(index)
            } else if (action === "jump") {
                Menus.preview = null
                root.openAbove(root.jumpList(task), task, true, { keyboard: true, minWidth: 256, rebuild: () => root.jumpList(task) })
            } else if (Menus.preview && Menus.preview.cycling && Menus.preview.button === task) {
                Menus.advancePreview()
            } else if (task.windowActive && task.windowCount > 1 && !task.attention &&
                       Config.taskbarClickActive !== "next") {
                root.openPreviews(task, true, true)
            } else {
                // As a click: in front with one window, it minimizes, so a
                // second press hides what the first brought.
                Menus.preview = null
                root.taskModel.activate(index)
            }
        }
    }

    // --- right-click menus (docs/renderer.md §14.4-14.6) ---

    // The bar's top-left on its output, and from its coordinates to the
    // output's.
    function barOrigin() {
        return Qt.point(position === "right" ? Screen.width - root.width : 0,
                        position === "bottom" ? Screen.height - root.height : 0)
    }
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        const o = barOrigin()
        return Qt.point(o.x + p.x, o.y + p.y)
    }
    // An item's middle along the bar, on the output: where its previews,
    // tooltip and drawer are centered.
    function centerOnScreen(item) {
        const p = screenPoint(item, item.width / 2, item.height / 2)
        return vertical ? p.y : p.x
    }

    // A menu from the bar's inner edge, at `item`'s point (x, y): rising
    // above a bottom bar, hanging below a top one, beside a vertical one;
    // `further` units into the screen (a menu over the drawer).
    function openFromBar(entries, item, x, y, options, further) {
        const p = screenPoint(item, x, y)
        openAtBar(entries, vertical ? p.y : p.x, options, further)
    }
    function openAtBar(entries, at, options, further) {
        const o = Object.assign({}, options || {})
        const d = 4 + (further || 0)
        if (position === "top") {
            root.openMenu(entries, at, root.height + d, o)
        } else if (position === "left") {
            delete o.centered
            root.openMenu(entries, root.width + d, at, o)
        } else if (position === "right") {
            delete o.centered
            root.openMenu(entries, Screen.width - root.width - d, at, Object.assign(o, { before: true }))
        } else {
            root.openMenu(entries, at, Screen.height - root.height - d, Object.assign(o, { above: true }))
        }
    }
    function openAbove(entries, item, centered, options) {
        openFromBar(entries, item, centered && !vertical ? item.width / 2 : 0, 0,
            Object.assign({ centered: centered }, options || {}))
    }
    // Taskbar menus open on its output; the panels it opens (Start, quick
    // settings...) go there too.
    function openMenu(entries, x, y, options) {
        Menus.panelScreen = root.screenName
        Menus.open(entries, x, y, Object.assign({ screen: root.screenName }, options || {}))
    }
    function fromHere() { Menus.panelScreen = root.screenName }

    // In a centered pill (fit, not at the edge) the clock and the system
    // icons are near the middle: their panels open above them (Menus).
    readonly property var trayAnchors: fit && !edge
        ? { clock: clock.x + clock.width / 2, icons: systemIcons.visible ? systemIcons.x + systemIcons.width / 2 : undefined }
        : null
    onTrayAnchorsChanged: Menus.setTrayAnchors(root.screenName, trayAnchors)
    onScreenNameChanged: Menus.setTrayAnchors(root.screenName, trayAnchors)
    Component.onDestruction: Menus.setTrayAnchors(root.screenName, null)

    // A button's jump list: pinned and recent files, the app's tasks, then the
    // app, pin/unpin, close.
    function jumpList(task) {
        const id = task.desktopId
        const entries = []
        if (id !== "") {
            const fileEntry = (file, pinned) => ({
                key: pinned ? "taskbar.pinned" : "taskbar.recent",
                text: file.name.replace(/&/g, "&&"),
                icon: file.icon,
                action: () => Apps.launchWithFile(id, file.url),
                pin: { pinned: pinned, toggle: () => Jumps.setPinned(id, file.url, !pinned) },
                context: [
                    { text: qsTr("&Open"), icon: task.iconName, action: () => Apps.launchWithFile(id, file.url) },
                    pinned
                        ? { text: qsTr("&Remove from this list"), icon: "window-unpin", action: () => Jumps.setPinned(id, file.url, false) }
                        : { text: qsTr("&Pin to this list"), icon: "window-pin", action: () => Jumps.setPinned(id, file.url, true) },
                    { text: qsTr("Remove from &list"), icon: "list-remove", action: () => Jumps.forget(id, file.url) }
                ]
            })
            const pinned = Jumps.pinned(id)
            if (pinned.length > 0) {
                entries.push({ key: "taskbar.pinned", header: qsTr("Pinned") })
                pinned.forEach(f => entries.push(fileEntry(f, true)))
            }
            const recent = Jumps.recent(id, 10)
            if (recent.length > 0) {
                entries.push({ key: "taskbar.recent", header: qsTr("Recent") })
                recent.forEach(f => entries.push(fileEntry(f, false)))
            }
            const actions = Apps.actions(id)
            if (actions.length > 0) {
                entries.push({ key: "taskbar.tasks", header: qsTr("Tasks") })
                actions.forEach(a => entries.push({ key: "taskbar.tasks", text: a.name.replace(/&/g, "&&"), icon: a.icon, action: () => Apps.launchAction(id, a.id) }))
            }
            if (entries.length > 0) {
                entries.push({ separator: true })
            }
            entries.push({ key: "taskbar.launch", text: task.name.replace(/&/g, "&&"), icon: task.iconName, action: () => Apps.launchId(id) })
            entries.push(Tasks.isPinned(id)
                ? { key: "taskbar.pin", text: qsTr("Unpin from taskbar"), icon: "window-unpin", action: () => Tasks.unpin(id) }
                : { key: "taskbar.pin", text: qsTr("Pin to taskbar"), icon: "window-pin", action: () => Tasks.pin(id) })
        }
        if (task.hasWindows && Config.endTask) {
            entries.push({ text: qsTr("End task"), icon: "process-stop", action: () => root.taskModel.endTask(task.index) })
        }
        if (task.windowCount > 0) {
            entries.push({
                key: "taskbar.close",
                text: task.windowCount > 1 ? qsTr("Close all windows") : qsTr("Close window"),
                icon: "window-close",
                action: () => root.taskModel.closeWindows(task.index)
            })
        }
        return entries
    }

    // Win+X: the Start button menu.
    function winXEntries() {
        const item = (key, text, icon, name) => ({ key: key, text: text, icon: icon, enabled: System.available(name), action: () => System.trigger(name) })
        const entries = [item("winx.installedApps", qsTr("Insta&lled apps"), "view-app-grid-symbolic", "installed-apps")]
        if (System.isLaptop()) {
            entries.push(item("winx.mobility", qsTr("Mobility &Center"), "computer-laptop-symbolic", "mobility"))
        }
        entries.push(
            item("winx.power", qsTr("P&ower options"), "battery-profile-powersave-symbolic", "power"),
            item("winx.events", qsTr("E&vent viewer"), "view-list-text-symbolic", "events"),
            item("winx.system", qsTr("S&ystem"), "computer-symbolic", "system"),
            item("winx.network", qsTr("Net&work connections"), "preferences-system-network-symbolic", "network"),
            { separator: true },
            item("winx.terminal", qsTr("&Terminal"), "utilities-terminal-symbolic", "terminal"),
            item("winx.terminalAdmin", qsTr("Terminal (&Admin)"), "utilities-terminal-symbolic", "terminal-admin"),
            { separator: true },
            item("winx.taskManager", qsTr("Task manage&r"), "office-chart-area", "task-manager"),
            item("winx.settings", qsTr("Setti&ngs"), "preferences-system-symbolic", "settings"),
            item("winx.files", qsTr("File &Explorer"), "system-file-manager-symbolic", "files"),
            { key: "winx.search", text: qsTr("&Search"), icon: "search", action: () => { root.fromHere(); if (!Shell.startMenuOpen) Shell.toggleStartMenu() } },
            { separator: true },
            { key: "winx.shutdown", text: qsTr("Sh&ut down or sign out"), icon: "system-shutdown", children: [
                { text: qsTr("Sign &out"), icon: "system-log-out", action: () => Shell.logout() },
                { text: qsTr("S&leep"), icon: "system-suspend", enabled: Shell.canSuspend(), action: () => Shell.suspend() },
                { text: qsTr("Sh&ut down"), icon: "system-shutdown", action: () => Shell.powerOff() },
                { text: qsTr("&Restart"), icon: "system-reboot", action: () => Shell.reboot() }
            ] },
            { key: "winx.desktop", text: qsTr("&Desktop"), icon: "user-desktop-symbolic", action: () => Tasks.toggleDesktop() }
        )
        return entries
    }

    Connections {
        target: Shell
        enabled: root.primary
        function onWinXRequested() {
            root.openWinX(true)
        }
        function onShowDesktopRequested() {
            Tasks.toggleDesktop()
        }
    }
    function openWinX(keyboard) {
        root.openFromBar(winXEntries(), startButton, 0, 0, { keyboard: keyboard })
    }

    // The menu of a notification area icon (the app decides it).
    Connections {
        target: Tray
        enabled: root.primary
        function onMenuReady(row, anchorX, entries) {
            const fromOverflow = !!Menus.trayOverflow
            const icon = Tray.entries.find(item => item.row === row)
            const convert = list => list.map(e => e.separator ? { separator: true } : {
                text: e.label,
                icon: e.icon,
                enabled: e.enabled,
                checked: e.checkable ? e.checked : undefined,
                radio: e.radio,
                children: e.children.length > 0 ? convert(e.children) : undefined,
                action: () => {
                    Menus.trayOverflow = null
                    Tray.activateMenuEntry(row, e.id)
                }
            })
            const all = convert(entries)
            // The shell's own entry, after the app's (docs/taskbar.md §8.2).
            if (icon && icon.key !== "") {
                const shown = root.trayIcons.shown.some(item => item.row === row)
                if (all.length > 0) all.push({ separator: true })
                all.push(shown
                    ? { key: "tray.drawer", text: qsTr("&Hide in the drawer"), icon: "go-up", action: () => { Menus.trayOverflow = null; root.placeTrayIcon(icon.key, false) } }
                    : { key: "tray.drawer", text: qsTr("&Show on the taskbar"), icon: "go-down", action: () => { Menus.trayOverflow = null; root.placeTrayIcon(icon.key, true) } })
            }
            if (all.length === 0) return
            root.openAtBar(all, anchorX, { centered: true, onClosed: () => { Menus.trayOverflow = null } },
                fromOverflow ? 40 : 0)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: root.floating ? "transparent" : Theme.taskbar

        // A floating style's pill (the apps', with fit or edge).
        Rectangle {
            id: floatingSurface
            visible: root.floating
            // On the screen's edge: the bottom one, or the top on a top bar.
            readonly property bool flush: root.edge && root.style.flush
            x: root.vertical ? root.style.margin : root.fit ? root.buttonsX - root.fitPadding : root.sideMargin
            y: root.vertical ? root.style.margin : flush && root.atTop ? 0 : root.style.margin
            width: root.vertical ? parent.width - 2 * root.style.margin
                : root.split ? buttons.width + 14
                : root.edge ? buttons.width + 20
                : root.fit ? buttons.width + root.fitGap + root.trayGroupWidth + 2 * root.fitPadding
                : parent.width - 2 * root.sideMargin
            height: root.vertical ? parent.height - 2 * root.style.margin
                : parent.height - (flush ? 1 : 2) * root.style.margin
            radius: root.style.radius
            topLeftRadius: flush && root.atTop ? 0 : radius
            topRightRadius: flush && root.atTop ? 0 : root.split ? root.style.splitRadius : radius
            bottomRightRadius: flush && !root.atTop ? 0 : root.split ? root.style.splitRadius : radius
            bottomLeftRadius: flush && !root.atTop ? 0 : radius
            color: Theme.taskbar
            border.width: 1
            border.color: root.style.stroke
            onXChanged: root.updateBlur()
            onYChanged: root.updateBlur()
            onWidthChanged: root.updateBlur()
            onHeightChanged: root.updateBlur()
        }
        // The tray's own pill (split-gap, or edge at the screen's edge).
        Rectangle {
            visible: root.split || root.edge
            x: root.edge ? parent.width - root.edgeGap - 12 - root.trayGroupWidth - 10
                : floatingSurface.x + floatingSurface.width + root.style.splitGap
            y: floatingSurface.flush ? 3 : floatingSurface.y
            width: root.edge ? root.trayGroupWidth + 22
                : root.buttonsX + buttons.width + root.fitGap + root.trayGroupWidth + root.fitPadding - x
            height: floatingSurface.flush ? parent.height - 6 : floatingSurface.height
            radius: root.style.radius
            topLeftRadius: root.edge ? radius : root.style.splitRadius
            bottomLeftRadius: topLeftRadius
            color: Theme.taskbar
            border.width: 1
            border.color: root.style.stroke
        }

        // Empty space: Task Manager and taskbar settings.
        MouseArea {
            id: emptyArea
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: mouse => {
                root.openFromBar([
                    { text: qsTr("Task &manager"), icon: "office-chart-area", enabled: System.available("task-manager"), action: () => System.trigger("task-manager") },
                    { text: qsTr("Taskbar &settings"), icon: "configure", enabled: System.available("taskbar-settings"), action: () => System.trigger("taskbar-settings") }
                ], emptyArea, mouse.x, mouse.y)
            }
        }

        // A thin line of light on the edge facing the windows, like glass.
        Rectangle {
            visible: !root.floating
            x: root.position === "left" ? parent.width - 1 : 0
            y: root.atTop ? parent.height - 1 : 0
            width: root.vertical ? 1 : parent.width
            height: root.vertical ? parent.height : 1
            color: root.style.stroke
        }
    }

    TapHandler {
        gesturePolicy: TapHandler.DragThreshold
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        function hits(item) {
            if (!item || item.Window.window !== root) return false
            const p = item.mapFromItem(root.contentItem, point.position.x, point.position.y)
            return item.contains(p)
        }
        onPressedChanged: {
            if (!pressed) return
            if (Menus.preview && Menus.preview.latched && !hits(Menus.preview.button)) Menus.preview = null
            if (Menus.trayOverflow && !hits(overflowButton)) Menus.trayOverflow = null
        }
    }

    // Clip moving and dragged buttons too: their click targets must never
    // enter the tray while a row animation or a resize is in progress.
    Item {
        id: buttonClip
        objectName: "taskbarButtonClip"
        x: root.vertical ? 0 : root.buttonsLeft
        y: root.vertical ? root.buttonsTop : 0
        // Up to the first of the tray's pieces.
        readonly property Item next: root.primary ? (transferIndicator.visible ? transferIndicator : tray) : clock
        width: root.vertical ? parent.width
            : Math.min(root.buttonsAreaWidth, Math.max(0, next.x - x - 4))
        height: root.vertical ? Math.max(0, Math.min(root.buttonsBottom, next.y - 4) - y) : parent.height
        clip: true
        Grid {
            id: buttons
            objectName: "taskbarButtons"
            // A row, or a column on a vertical bar.
            rows: root.vertical ? -1 : 1
            columns: root.vertical ? 1 : -1
            // Centered by hand instead of with anchors: so when an app opens or
            // closes, the row slides to the new center instead of jumping. Or on
            // the left, like earlier versions of Windows (the top, on a
            // vertical bar).
            x: root.vertical ? Math.round((buttonClip.width - width) / 2)
                : Math.max(root.buttonsLeft, Math.min(root.buttonsRight - width, preferredX)) - buttonClip.x
            y: root.vertical ? Math.max(root.buttonsTop, Math.min(root.buttonsBottom - height, preferredY)) - buttonClip.y
                : Math.round((buttonClip.height - height) / 2)
            readonly property real preferredX: root.edge && root.style.edge === 2 ? root.style.margin + 12
                : root.fit && !root.edge ? Math.round((root.width - width - root.fitGap - root.trayGroupWidth) / 2)
                : !root.edge && Config.taskbarAlignment === "left" ? root.buttonsLeft
                : Math.round((root.width - width) / 2)
            readonly property real preferredY: Config.taskbarAlignment === "left" ? root.buttonsTop
                : Math.round((root.height - height) / 2)
            spacing: root.style.spacing

            Behavior on x {
                enabled: Theme.motion > 0 && !root.vertical
                Eased {}
            }
            Behavior on y {
                enabled: Theme.motion > 0 && root.vertical
                Eased {}
            }
            // The button of an app just opened comes in growing.
            add: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal }
                Eased { property: "scale"; from: 1 - 0.5 * Theme.motion; to: 1; duration: Theme.slow }
            }
            move: Transition {
                Eased { properties: "x,y"; duration: Theme.normal * Theme.motion }
            }

            TaskbarButton {
                id: startButton
                objectName: "startButton"
                active: Shell.startMenuOpen
                onClicked: { root.fromHere(); Shell.toggleStartMenu() }
                onRightClicked: root.openWinX(false)
                // Vela's sail, the distribution's logo or a picture (Settings > Start).
                StartIcon { anchors.centerIn: parent; source: Config.startIconSource; revision: Config.startIconRevision }
            }

            // Task View: two overlapping windows, like the Windows 11 icon.
            TaskbarButton {
                visible: Config.taskView
                tooltip: qsTr("Task view")
                onClicked: { root.fromHere(); Shell.taskViewRequested() }
                Item {
                    anchors.centerIn: parent
                    width: 20
                    height: 18
                    Rectangle {
                        x: 0
                        y: 4
                        width: 13
                        height: 13
                        radius: 2.5
                        color: "transparent"
                        border.width: 1.5
                        border.color: Theme.text
                    }
                    Rectangle {
                        x: 6
                        y: 0
                        width: 13
                        height: 13
                        radius: 2.5
                        color: Theme.accentFill
                        opacity: 0.9
                    }
                }
            }

            TaskbarApps {
                id: appStrip
                objectName: "taskbarApps"
                style: root.style
                vertical: root.vertical
                visible: taskRepeater.count > 0
                width: root.vertical ? root.style.buttonWidth
                    : Math.min(implicitWidth, Math.max(0, root.buttonsAreaWidth - root.fixedButtonsWidth - buttons.spacing))
                height: root.vertical
                    ? Math.min(implicitHeight, Math.max(0, root.buttonsBottom - root.buttonsTop - root.fixedButtonsHeight - buttons.spacing))
                    : root.style.buttonHeight
                onContentXChanged: {
                    previewDelay.stop()
                    Menus.preview = null
                }

                Repeater {
                    id: taskRepeater
                    model: root.taskModel

                    TaskbarButton {
                        id: task
                        objectName: "taskButton-" + index
                        draggable: true
                        // Dropped further away: it moves by as many places as buttons
                        // it passed.
                        onDropped: dx => {
                            const step = (root.vertical ? height : width) + buttons.spacing
                            const to = Math.max(0, Math.min(taskRepeater.count - 1, index + Math.round(dx / step)))
                            if (to !== index) root.taskModel.move(index, to)
                        }
                        onDraggingChanged: if (dragging) Menus.preview = null
                        required property int index
                        required property string key
                        required property string name
                        required property string iconName
                        required property int windowCount
                        required property bool windowActive
                        required property string desktopId
                        required property var windowIds
                        required property bool attention
                        required property string activeWindowId
                        required property bool pinned
                        required property bool hasWindows
                        dropHighlighted: appDrop.containsDrag && appDrop.hoveredButton === task && !appDrop.pinning
                        required property string badgeCount
                        required property real progress
                        countBadge: badgeCount
                        progressValue: progress
                        shortcutNumber: root.showNumberHints && Config.taskbarNumberHints && index < 10
                            ? ((index + 1) % 10).toString() : ""

                        tooltip: attention ? qsTr("%1 needs attention").arg(name) : name
                        Accessible.role: Accessible.Button
                        Accessible.name: tooltip
                        Accessible.onPressAction: root.activateButton(task)
                        needsAttention: attention
                        flashAttention: Theme.animationEffects && !Notifications.silenced
                        running: windowCount > 0
                        active: windowActive
                        onClicked: root.activateButton(task)
                        onMiddleClicked: root.taskModel.launchNew(index)
                        // The wheel: the volume of this app only, if it's playing
                        // sound, with the indicator. A step every notch (120),
                        // touchpads add up their small ones.
                        property real wheelRest: 0
                        onWheeled: delta => {
                            wheelRest += delta
                            while (Math.abs(wheelRest) >= 120) {
                                root.fromHere()
                                if (!Sound.stepTaskVolume(desktopId, wheelRest > 0 ? 1 : -1)) {
                                    wheelRest = 0
                                    return
                                }
                                wheelRest -= wheelRest > 0 ? 120 : -120
                            }
                        }
                        // The previews of its windows, with the mouse resting on the
                        // button (at once, if another button's are already open).
                        onHoveredChanged: {
                            Menus.previewButtonHovered = hovered
                            if (Menus.preview && Menus.preview.latched) return
                            if (hovered && appStrip.fullyVisible(task)) {
                                previewDelay.task = task
                                if (Menus.preview) {
                                    previewDelay.triggered()
                                } else {
                                    previewDelay.restart()
                                }
                            } else {
                                if (previewDelay.task === task) previewDelay.stop()
                                if (Menus.tooltip && Menus.tooltip.owner === task) Menus.tooltip = null
                            }
                        }
                        onRightClicked: shift => {
                            Menus.preview = null
                            if ((shift || Shell.shiftHeld()) && windowCount > 0) {
                                const state = root.taskModel.windowState(index)
                                root.openAbove(Menus.windowEntries(state.maximized, state.minimized, true,
                                    action => root.taskModel.windowAction(task.index, action)), task, true)
                            } else {
                                root.openAbove(root.jumpList(task), task, true, { minWidth: 256, rebuild: () => root.jumpList(task) })
                            }
                        }

                        // The compositor flies minimized windows here.
                        function reportGeometry() {
                            if (windowCount > 0) {
                                root.taskModel.setButtonGeometry(index, root, appStrip.buttonGeometry(task, root.contentItem))
                            }
                        }
                        onXChanged: reportGeometry()
                        onYChanged: reportGeometry()
                        onWindowCountChanged: reportGeometry()
                        onActiveChanged: if (active) Qt.callLater(() => appStrip.reveal(task))
                        Component.onCompleted: if (active) Qt.callLater(() => appStrip.reveal(task))
                        Component.onDestruction: {
                            if (Menus.preview && Menus.preview.button === task) Menus.preview = null
                            if (Menus.tooltip && Menus.tooltip.owner === task) Menus.tooltip = null
                        }
                        Connections {
                            target: appStrip
                            function onGeometryChanged() { task.reportGeometry() }
                        }
                        // The row moves down too, when a floating bar grows taller.
                        Connections {
                            target: buttons
                            function onXChanged() { task.reportGeometry() }
                            function onYChanged() { task.reportGeometry() }
                        }
                        Connections {
                            target: buttonClip
                            function onXChanged() { task.reportGeometry() }
                            function onYChanged() { task.reportGeometry() }
                        }

                        Image {
                            anchors.fill: parent
                            source: Theme.icons + encodeURIComponent(task.iconName)
                            sourceSize: Qt.size(width, height)
                            smooth: true
                            mipmap: true
                        }
                    }
                }
            }
        }
    }

    // Transfers remain tracked by FileOps; the shell only aggregates status.
    Item {
        id: transferIndicator
        objectName: "transferIndicator"
        visible: root.primary && Transfers.activeCount > 0
        anchors { right: tray.left; top: parent.top; bottom: parent.bottom; rightMargin: 4 }
        width: visible ? 68 : 0
        readonly property var running: Transfers.jobs.filter(job => !job.finished)
        readonly property real progress: {
            if (running.length !== 1 || running[0].total <= 0) return -1
            return Math.max(0, Math.min(100, Math.round(100 * running[0].done / running[0].total)))
        }
        function readableSize(bytes) {
            if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(1) + " GB"
            if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + " MB"
            if (bytes >= 1024) return (bytes / 1024).toFixed(1) + " KB"
            return Math.round(bytes) + " B"
        }
        function jobById(owner, id) {
            return Transfers.jobs.find(job => job.owner === owner && job.id === id)
        }
        function openTransferMenu() {
            const entries = [{ header: qsTr("File transfers") }]
            for (const initial of running) {
                const owner = initial.owner
                const id = initial.id
                entries.push({
                    text: initial.title, enabled: false,
                    liveText: () => {
                        const j = jobById(owner, id)
                        if (!j) return qsTr("Transfer removed")
                        return j.title + " · " + (j.finished ? qsTr("Finished")
                            : j.total > 0 ? Math.round(100 * j.done / j.total) + "%" : qsTr("Preparing"))
                    }
                })
                entries.push({
                    text: initial.current || qsTr("Preparing"), enabled: false,
                    liveText: () => {
                        const j = jobById(owner, id)
                        return j ? (j.current || (j.finished ? qsTr("Finished") : qsTr("Preparing"))) : ""
                    }
                })
                entries.push({
                    text: qsTr("Bytes transferred"), enabled: false,
                    liveText: () => {
                        const j = jobById(owner, id)
                        return j ? readableSize(j.done) + " / " +
                            (j.total > 0 ? readableSize(j.total) : qsTr("calculating")) : ""
                    }
                })
                entries.push({
                    text: qsTr("Calculating speed"), enabled: false,
                    liveText: () => {
                        const j = jobById(owner, id)
                        if (!j) return ""
                        if (j.finished) return qsTr("Transfer finished")
                        if (j.speed <= 0) return qsTr("Calculating transfer speed…")
                        const eta = j.eta < 0 ? qsTr("time unknown")
                            : j.eta < 60 ? qsTr("%1 seconds remaining").arg(Math.ceil(j.eta))
                            : qsTr("%1 minutes remaining").arg(Math.ceil(j.eta / 60))
                        return readableSize(j.speed) + "/s · " + eta
                    }
                })
                entries.push({ text: qsTr("Cancel transfer"), icon: "process-stop",
                               action: () => Transfers.cancel(owner, id) })
                entries.push({ separator: true })
            }
            root.openAbove(entries, transferIndicator, true, { minWidth: 360 })
        }
        Fill {
            anchors {
                fill: parent
                topMargin: root.vertical ? 2 : 8 + root.inset
                bottomMargin: root.vertical ? 2 : 8 + root.inset
                leftMargin: root.vertical ? 6 : 0
                rightMargin: root.vertical ? 6 : 0
            }
            radius: root.style.corner(8, Theme.radius)
            color: transferMouse.pressed ? Theme.subtlePressed
                : transferMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
        }
        // Side by side; one above the other on a vertical bar.
        Icon {
            x: root.vertical ? Math.round((parent.width - width) / 2) : 7
            y: root.vertical ? 6 : Math.round((parent.height - height) / 2)
            name: "document-save"
            size: 16
        }
        Text {
            x: root.vertical ? Math.round((parent.width - width) / 2) : parent.width - width - 6
            y: root.vertical ? parent.height - height - 5 : Math.round((parent.height - height) / 2)
            text: transferIndicator.progress >= 0 ? transferIndicator.progress + "%"
                : String(Transfers.activeCount)
            color: Theme.text
            font.pixelSize: Theme.fontCaption
        }
        MouseArea {
            id: transferMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: transferIndicator.openTransferMenu()
        }
    }

    // This area asks for drag motion so hovering can raise a window, but
    // accepts a drop only when a single launcher is pinned as a copy.
    DropArea {
        id: appDrop
        // After Start and Task View, along the bar up to the tray.
        readonly property point start: buttons.mapToItem(root.contentItem, 0, 0)
        x: root.vertical ? 0 : root.buttonsX + root.fixedButtonsWidth
        y: root.vertical ? start.y + root.fixedButtonsHeight : start.y
        width: root.vertical ? root.width : Math.max(0, root.buttonsRight - x)
        height: root.vertical ? Math.max(0, root.buttonsBottom - y) : buttons.height
        property bool pinning: false
        property var hoveredButton: null
        property int insertRow: -1
        property real markerX: 0

        // `px` along the bar, in the area's coordinates.
        function updatePosition(px) {
            const position = px + (root.vertical ? y : x)
            let hit = null
            let insertBefore = -1
            let marker = x
            let lastPinnedEdge = -1
            let lastPinnedRow = -1
            let firstAppEdge = -1
            let firstAppRow = -1
            for (let i = 0; i < taskRepeater.count; ++i) {
                const task = taskRepeater.itemAt(i)
                if (!task || !appStrip.fullyVisible(task)) continue
                const left = root.along(task)
                const right = left + root.lengthOf(task)
                if (firstAppEdge < 0) {
                    firstAppEdge = left
                    firstAppRow = i
                }
                if (position >= left && position < right) hit = task
                if (task.pinned) {
                    lastPinnedEdge = right
                    lastPinnedRow = i
                    if (insertBefore < 0 && position < (left + right) / 2) {
                        insertBefore = i
                        marker = left
                    }
                }
            }
            if (insertBefore < 0 && lastPinnedEdge >= 0) marker = lastPinnedEdge
            else if (insertBefore < 0 && firstAppEdge >= 0) marker = firstAppEdge
            // After the last shown pinned app, the drop still goes before the
            // pinned apps paged out after it: -1 (append) only past them all.
            if (insertBefore < 0) {
                for (let j = lastPinnedRow >= 0 ? lastPinnedRow + 1 : Math.max(0, firstAppRow); j < taskRepeater.count; ++j) {
                    const next = taskRepeater.itemAt(j)
                    if (next && next.pinned) {
                        insertBefore = j
                        break
                    }
                }
            }
            insertRow = insertBefore
            markerX = Math.max(0, Math.min((root.vertical ? height : width) - 2, marker - (root.vertical ? y : x)))
            if (hoveredButton !== hit) {
                springTimer.stop()
                if (Menus.preview && Menus.preview.spring) Menus.preview = null
                hoveredButton = hit
                if (!pinning && hit && hit.windowCount > 0) springTimer.start()
            }
        }
        onEntered: drag => {
            pinning = drag.hasUrls && drag.urls.length === 1 && !!(drag.supportedActions & Qt.CopyAction)
                && root.taskModel.isLauncherDrop(drag.urls[0])
            hoveredButton = null
            updatePosition(root.vertical ? drag.y : drag.x)
        }
        onPositionChanged: drag => updatePosition(root.vertical ? drag.y : drag.x)
        onExited: {
            if (Menus.preview && Menus.preview.spring) Menus.previewButtonHovered = false
            hoveredButton = null
            springTimer.stop()
        }
        onDropped: drop => {
            springTimer.stop()
            hoveredButton = null
            if (Menus.preview && Menus.preview.spring) Menus.preview = null
            if (pinning && root.taskModel.pinDropped(drop.urls[0], insertRow)) {
                drop.accept(Qt.CopyAction)
            } else {
                drop.accepted = false
            }
        }
        Timer {
            id: springTimer
            interval: 600
            onTriggered: {
                const task = appDrop.hoveredButton
                if (!task || !appDrop.containsDrag) return
                if (task.windowCount === 1) root.taskModel.raiseWindow(task.index)
                else if (task.windowCount > 1) {
                    Menus.previewButtonHovered = true
                    root.openPreviews(task, false, false, true)
                }
            }
        }
        Rectangle {
            visible: appDrop.containsDrag && appDrop.pinning
            x: root.vertical ? 3 : appDrop.markerX
            y: root.vertical ? appDrop.markerX : 3
            width: root.vertical ? parent.width - 6 : 2
            height: root.vertical ? 2 : parent.height - 6
            radius: 1
            color: Theme.accent
        }
    }

    // The main taskbar's notification area, left of the system icons.
    Grid {
        id: tray
        objectName: "taskbarTray"
        visible: root.primary && (root.trayIcons.shown.length > 0 || root.hiddenTrayIcons.length > 0)
        rows: root.vertical ? -1 : 1
        columns: root.vertical ? 1 : -1
        anchors { right: screenSharing.left; top: parent.top; bottom: parent.bottom; rightMargin: 4 }

        TaskbarButton {
            id: overflowButton
            objectName: "trayOverflowButton"
            visible: root.hiddenTrayIcons.length > 0 || root.draggedTrayKey !== ""
            width: !visible ? 0 : root.vertical ? root.style.buttonWidth : root.traySlotWidth
            height: !visible ? 0 : root.vertical ? root.traySlotWidth : root.style.buttonHeight
            y: root.vertical ? 0 : Math.round((tray.height - height) / 2)
            active: !!Menus.trayOverflow || (root.draggedTrayKey !== "" && root.trayMarkerX < 0)
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Show hidden icons")
            Accessible.onPressAction: clicked()
            onClicked: {
                if (Menus.trayOverflow) {
                    Menus.trayOverflow = null
                } else {
                    Menus.trayOverflow = { bar: root, center: root.centerOnScreen(overflowButton), screen: root.screenName }
                }
            }
            // Pointing to where the drawer opens, back when it is open.
            Text {
                anchors.centerIn: parent
                readonly property real towards: root.atTop ? 90 : root.position === "left" ? 0
                    : root.position === "right" ? 180 : -90
                rotation: overflowButton.active ? towards + 180 : towards
                text: "›"
                color: Theme.text
                font.pixelSize: 19
            }
        }

        Repeater {
            id: trayRepeater
            model: root.trayIcons.shown
            delegate: TrayIcon {
                id: trayItem
                required property var modelData
                required property int index
                objectName: "trayIcon-" + index
                entry: modelData
                vertical: root.vertical
                centerOnScreen: () => root.centerOnScreen(trayItem)
                width: root.vertical ? root.width - 2 * root.inset : root.traySlotWidth
                height: root.vertical ? root.traySlotWidth : tray.height
                inset: root.vertical ? 6 : 8 + root.inset
                draggable: entry.key !== ""
                onDragMoved: p => root.trayDragMoved(entry.key, p.x, p.y)
                onDropped: p => root.trayDropped(entry.key, p.x, p.y)
                onDragCanceled: root.trayDragCanceled()
                onInvoked: button => {
                    const center = root.centerOnScreen(trayItem)
                    if (button === Qt.RightButton) Tray.requestMenu(entry.row, center, true)
                    else if (button === Qt.MiddleButton) Tray.secondaryActivate(entry.row)
                    else Tray.activate(entry.row, center)
                }
            }
        }
    }
    // Where a dragged icon would land among the shown ones.
    Rectangle {
        visible: root.draggedTrayKey !== "" && root.trayMarkerX >= 0
        x: root.vertical ? tray.x + 6 : root.trayMarkerX - 1
        y: root.vertical ? root.trayMarkerX - 1 : tray.y + 8 + root.inset
        width: root.vertical ? tray.width - 12 : 2
        height: root.vertical ? 2 : tray.height - 2 * (8 + root.inset)
        radius: 1
        color: Theme.accent
    }

    // The screen being shared or recorded (docs/screen-sharing.md §6): a
    // screen with a red dot, who and what in the tooltip, and a menu listing
    // each app; picking one brings it forward, where its own "Stop sharing" is.
    Item {
        id: screenSharing
        objectName: "screenSharing"
        visible: root.primary && Sharing.active
        anchors { right: microphone.left; top: parent.top; bottom: parent.bottom }
        width: visible ? 32 : 0

        function showTooltip() {
            Menus.tooltip = { text: Sharing.summary, center: root.centerOnScreen(screenSharing), screen: root.screenName, owner: "screenSharing" }
        }
        function hideTooltip() {
            sharingTooltipDelay.stop()
            if (Menus.tooltip && Menus.tooltip.owner === "screenSharing") {
                Menus.tooltip = null
            }
        }
        function show(user) {
            if (!(user.desktopId && Tasks.showApp(user.desktopId)) && user.window) {
                Shell.windowAction(user.window, "activate")
            }
        }
        function openMenu() {
            const entries = [{ header: qsTr("Screen sharing") }]
            for (const user of Sharing.users) {
                entries.push({ text: user.name.replace(/&/g, "&&"), icon: user.icon, shortcut: user.what,
                               action: () => screenSharing.show(user) })
            }
            // Held back meanwhile (§7), unless the user's own switch is on.
            if (Notifications.automaticDoNotDisturb && !Notifications.doNotDisturb) {
                entries.push({ separator: true },
                             { header: qsTr("Notifications are silenced meanwhile") },
                             { text: qsTr("&Notification settings"), icon: "preferences-desktop-notification-symbolic",
                               action: () => System.trigger("notification-settings") })
            }
            root.openAbove(entries, screenSharing, true, { minWidth: 260 })
        }
        Connections {
            target: Sharing
            function onSummaryChanged() {
                if (Menus.tooltip && Menus.tooltip.owner === "screenSharing") {
                    screenSharing.showTooltip()
                }
            }
        }
        onVisibleChanged: if (!visible) hideTooltip()

        Timer {
            id: sharingTooltipDelay
            interval: 500
            onTriggered: screenSharing.showTooltip()
        }
        Fill {
            anchors {
                fill: parent
                topMargin: root.vertical ? 2 : 8 + root.inset
                bottomMargin: root.vertical ? 2 : 8 + root.inset
                leftMargin: root.vertical ? 6 : 0
                rightMargin: root.vertical ? 6 : 0
            }
            radius: root.style.corner(8, Theme.radius)
            color: sharingMouse.pressed ? Theme.subtlePressed : sharingMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
        }
        Icon {
            id: sharingIcon
            anchors.centerIn: parent
            name: "monitor"
        }
        // The red dot of "recording", at the screen's lower right.
        Rectangle {
            width: 7
            height: 7
            radius: 3.5
            color: "#e81123"
            border.width: 1
            border.color: Theme.taskbar
            x: sharingIcon.x + sharingIcon.width - 5
            y: sharingIcon.y + sharingIcon.height - 6
        }
        MouseArea {
            id: sharingMouse
            anchors.fill: parent
            hoverEnabled: true
            onContainsMouseChanged: containsMouse ? sharingTooltipDelay.restart() : screenSharing.hideTooltip()
            onClicked: {
                screenSharing.hideTooltip()
                screenSharing.openMenu()
            }
        }
    }

    // The microphone in use (docs/audio.md §7.2), like Windows: who listens
    // in the tooltip, a click opens Settings > Sound.
    Item {
        id: microphone
        objectName: "microphone"
        visible: root.primary && Audio.microphoneUsers.length > 0
        anchors { right: caffeine.left; top: parent.top; bottom: parent.bottom }
        width: visible ? 32 : 0

        readonly property string tooltipText: {
            const users = Audio.microphoneUsers
            if (users.length === 1) {
                return qsTr("%1 is using your microphone").arg(users[0].name)
            }
            return [qsTr("These apps are using your microphone:")].concat(users.map(u => u.name)).join("\n")
        }
        function showTooltip() {
            Menus.tooltip = { text: microphone.tooltipText, center: root.centerOnScreen(microphone), screen: root.screenName, owner: "microphone" }
        }
        function hideTooltip() {
            tooltipDelay.stop()
            if (Menus.tooltip && Menus.tooltip.owner === "microphone") {
                Menus.tooltip = null
            }
        }
        onTooltipTextChanged: if (Menus.tooltip && Menus.tooltip.owner === "microphone") showTooltip()
        onVisibleChanged: if (!visible) hideTooltip()

        Timer {
            id: tooltipDelay
            interval: 500
            onTriggered: microphone.showTooltip()
        }
        Fill {
            anchors {
                fill: parent
                topMargin: root.vertical ? 2 : 8 + root.inset
                bottomMargin: root.vertical ? 2 : 8 + root.inset
                leftMargin: root.vertical ? 6 : 0
                rightMargin: root.vertical ? 6 : 0
            }
            radius: root.style.corner(8, Theme.radius)
            color: microphoneMouse.pressed ? Theme.subtlePressed : microphoneMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
        }
        Icon {
            anchors.centerIn: parent
            name: "audio-input-microphone"
        }
        MouseArea {
            id: microphoneMouse
            anchors.fill: parent
            hoverEnabled: true
            onContainsMouseChanged: containsMouse ? tooltipDelay.restart() : microphone.hideTooltip()
            onClicked: {
                microphone.hideTooltip()
                System.trigger("sound-settings")
            }
        }
    }

    // Caffeine (keep awake, compositor/src/lock.c): always here, faint while
    // off. Either click lists the durations; the remaining time is in the
    // tooltip and at the top of the menu. Breeze's only cup is KTeaTime's.
    Item {
        id: caffeine
        objectName: "caffeine"
        visible: root.primary
        anchors { right: systemIcons.left; top: parent.top; bottom: parent.bottom }
        width: visible ? 32 : 0

        function remainingText(secs) {
            const minutes = Math.ceil(secs / 60)
            if (minutes < 60) {
                return minutes === 1 ? qsTr("1 minute remaining") : qsTr("%1 minutes remaining").arg(minutes)
            }
            if (minutes % 60 === 0) {
                return qsTr("%1 h remaining").arg(minutes / 60)
            }
            return qsTr("%1 h %2 min remaining").arg(Math.floor(minutes / 60)).arg(minutes % 60)
        }
        readonly property string tooltipText: !Access.caffeine ? qsTr("Caffeine: off")
            : Access.caffeineSecs > 0 ? qsTr("Caffeine: %1").arg(remainingText(Access.caffeineSecs))
            : qsTr("Caffeine: keeping the computer awake")
        function showTooltip() {
            Menus.tooltip = { text: caffeine.tooltipText, center: root.centerOnScreen(caffeine), screen: root.screenName, owner: "caffeine" }
        }
        function hideTooltip() {
            caffeineTooltipDelay.stop()
            if (Menus.tooltip && Menus.tooltip.owner === "caffeine") {
                Menus.tooltip = null
            }
        }
        onTooltipTextChanged: if (Menus.tooltip && Menus.tooltip.owner === "caffeine") showTooltip()
        onVisibleChanged: if (!visible) hideTooltip()

        // The selected preset, not the time left (which keeps decreasing).
        function openMenu() {
            const presets = [15, 30, 60, 120, 180, 300, 480]
            const names = [qsTr("15 minutes"), qsTr("30 minutes"), qsTr("1 hour"), qsTr("2 hours"),
                           qsTr("3 hours"), qsTr("5 hours"), qsTr("8 hours")]
            const entries = []
            if (Access.caffeine && Access.caffeineSecs > 0) {
                entries.push({ header: remainingText(Access.caffeineSecs) })
            }
            entries.push({ text: qsTr("&Indefinitely"), radio: true,
                           checked: Access.caffeine && Access.caffeineSecs < 0,
                           action: () => Access.caffeine = true },
                         { separator: true })
            for (let i = 0; i < presets.length; i++) {
                const minutes = presets[i]
                entries.push({ text: names[i], radio: true,
                               checked: Access.caffeine && Access.caffeineDurationSecs === minutes * 60,
                               action: () => Access.setCaffeineMinutes(minutes) })
            }
            entries.push({ separator: true },
                         { text: qsTr("Turn &off"), enabled: Access.caffeine, action: () => Access.caffeine = false },
                         { text: qsTr("&Power settings"), icon: "battery-profile-powersave-symbolic",
                           enabled: System.available("power"), action: () => System.trigger("power") })
            root.openAbove(entries, caffeine, true, { minWidth: 220 })
        }

        Timer {
            id: caffeineTooltipDelay
            interval: 500
            onTriggered: caffeine.showTooltip()
        }
        Fill {
            anchors {
                fill: parent
                topMargin: root.vertical ? 2 : 8 + root.inset
                bottomMargin: root.vertical ? 2 : 8 + root.inset
                leftMargin: root.vertical ? 6 : 0
                rightMargin: root.vertical ? 6 : 0
            }
            radius: root.style.corner(8, Theme.radius)
            color: caffeineMouse.pressed ? Theme.subtlePressed : Theme.subtleHover
            opacity: caffeineMouse.containsMouse ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        }
        Image {
            anchors.centerIn: parent
            width: 16
            height: 16
            source: Theme.icons + "kteatime-symbolic"
            sourceSize: Qt.size(width, height)
            opacity: Access.caffeine ? 1 : 0.45
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        }
        MouseArea {
            id: caffeineMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onContainsMouseChanged: containsMouse ? caffeineTooltipDelay.restart() : caffeine.hideTooltip()
            onClicked: {
                caffeine.hideTooltip()
                caffeine.openMenu()
            }
        }
    }

    // The system icons (network, volume, battery) in a single button: it opens
    // quick settings, like on Windows 11. The wheel over the volume changes
    // it; the right button opens their menus (§14.5).
    Item {
        id: systemIcons
        objectName: "systemIcons"
        visible: root.primary
        anchors { right: clock.left; top: parent.top; bottom: parent.bottom; rightMargin: 2 }
        width: visible ? iconsRow.width + 16 : 0

        Fill {
            anchors {
                fill: parent
                topMargin: root.vertical ? 2 : 4 + root.inset
                bottomMargin: root.vertical ? 2 : 4 + root.inset
                leftMargin: root.vertical ? 4 : 0
                rightMargin: root.vertical ? 4 : 0
            }
            radius: root.style.corner(4, root.floating ? root.style.ownButtonRadius : Theme.radius)
            color: systemMouse.pressed ? Theme.subtlePressed : systemMouse.containsMouse || Menus.quickSettingsOpen ? Theme.subtleHover : Theme.subtleRest
        }
        Grid {
            id: iconsRow
            anchors.centerIn: parent
            rows: root.vertical ? -1 : 1
            columns: root.vertical ? 1 : -1
            spacing: root.vertical ? 8 : 10
            Icon {
                id: networkIcon
                name: Status.networkIconName
            }
            Icon {
                id: volumeIcon
                visible: Audio.available
                name: Audio.iconName
            }
            Icon {
                visible: Status.batteryPresent
                name: Status.batteryCharging ? "battery-good-charging" : "battery-good"
            }
        }
        MouseArea {
            id: systemMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: mouse => {
                if (mouse.button === Qt.LeftButton) {
                    root.fromHere()
                    Shell.quickSettingsRequested()
                    return
                }
                // The menu of the icon under the mouse: volume or network.
                const at = root.vertical ? mouse.y : mouse.x
                const start = root.vertical ? volumeIcon.y + iconsRow.y : volumeIcon.x + iconsRow.x
                const overVolume = volumeIcon.visible && at >= start - 5
                    && at < start + (root.vertical ? volumeIcon.height : volumeIcon.width) + 5
                root.openFromBar(overVolume ? [
                    { text: qsTr("Open volume &mixer"), icon: "audio-volume-high", enabled: Audio.available, action: () => { root.fromHere(); Shell.soundOutputRequested() } },
                    { text: qsTr("&Sound settings"), icon: "audio-speakers-symbolic", enabled: System.available("sound-settings"), action: () => System.trigger("sound-settings") }
                ] : [
                    { text: qsTr("&Network and Internet settings"), icon: "preferences-system-network-symbolic", enabled: System.available("network"), action: () => System.trigger("network") }
                ], systemMouse, mouse.x, mouse.y)
            }
            // Like the keyboard's volume keys, with the indicator: a step every
            // notch (120), touchpads add up their small ones.
            property real wheelRest: 0
            onWheel: wheel => {
                wheelRest += wheel.angleDelta.y
                while (Math.abs(wheelRest) >= 120) {
                    root.fromHere()
                    Sound.press(wheelRest > 0 ? "up" : "down")
                    wheelRest -= wheelRest > 0 ? 120 : -120
                }
            }
        }
    }

    // Clock: time on top, date below (unless Settings hides it).
    Item {
        id: clock
        objectName: "taskbarClock"
        // At the right; with a floating style inside its pill, after the apps
        // (fit) or at the screen's edge (edge).
        x: root.edge ? parent.width - width - root.edgeGap - 12
            : root.fit ? root.buttonsX + buttons.width + root.fitGap + root.trayGroupWidth - width
            : parent.width - width - 12 - (root.floating ? root.sideMargin : 0)
        anchors { top: parent.top; bottom: parent.bottom }
        width: clockColumn.implicitWidth + 16 + (badge.visible ? 22 : 0)
        height: clockColumn.implicitHeight + 16 // on a vertical bar; a row's spans it

        property date now: Shell.now()

        // Just after each second begins, so the seconds (when shown) and the
        // minutes turn when they do.
        Timer {
            interval: 1000 - clock.now.getMilliseconds() + 5
            running: true
            repeat: true
            onTriggered: clock.now = Shell.now()
        }

        Fill {
            anchors {
                fill: parent
                topMargin: root.vertical ? 2 : 4 + root.inset
                bottomMargin: root.vertical ? 2 : 4 + root.inset
                leftMargin: root.vertical ? 4 : 0
                rightMargin: root.vertical ? 4 : 0
            }
            // A floating style's buttons' corners: its highlight is one of them.
            radius: root.style.corner(4, root.floating ? root.style.ownButtonRadius : Theme.radius)
            color: clockMouse.containsMouse || Menus.notificationCenterOpen ? Theme.subtleHover : Theme.subtleRest
        }
        // Notifications to read: a number next to the clock, like Windows 11,
        // unless Settings > Notifications hides it (with "Do not disturb",
        // the user's or while sharing the screen, a muted bell: that one is a
        // state, not a count, and stays).
        Rectangle {
            id: badge
            visible: (Config.notificationCount && Notifications.history.count > 0) || Notifications.silenced
            // Beside the time; at the column's top corner on a vertical bar.
            x: root.vertical ? parent.width - width + 2 : parent.width - width + 2
            y: root.vertical ? -2 : Math.round((parent.height - height) / 2)
            z: 1
            width: 18
            height: 18
            radius: 9
            color: Notifications.silenced ? "transparent" : Theme.accent
            Text {
                anchors.centerIn: parent
                visible: !Notifications.silenced
                text: Math.min(Notifications.history.count, 9)
                color: Theme.readableOn(Theme.accent, "white") // a light custom accent: black
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }
            Icon {
                anchors.centerIn: parent
                visible: Notifications.silenced
                name: "notifications-disabled"
                size: 14
            }
        }

        // On a vertical bar the lines are centered, AM/PM has a line of its
        // own and the date is the day and the month: the column is narrow.
        readonly property bool hasMeridiem: /a/i.test(Config.clockFormat)
        // The format's own AM/PM marker ("AP" or "ap"), formatted alone.
        readonly property string meridiem: {
            const marker = Config.clockFormat.match(/a+p*/i)
            return marker ? Qt.formatTime(now, marker[0]) : ""
        }
        Column {
            id: clockColumn
            x: root.vertical ? Math.round((parent.width - width) / 2) : 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 1

            Text {
                anchors.right: root.vertical ? undefined : parent.right
                anchors.horizontalCenter: root.vertical ? parent.horizontalCenter : undefined
                // 24-hour or 12-hour, with or without the seconds (Settings >
                // Taskbar). Digits all as wide, so the clock doesn't twitch
                // every second.
                objectName: "taskbarTime"
                // On a vertical bar AM/PM has a line of its own: formatted
                // with it (h is 1-12 only beside AP), then taken out.
                text: {
                    const full = Qt.formatTime(clock.now, Config.clockFormat)
                    return root.vertical && clock.meridiem ? full.replace(clock.meridiem, "").trim() : full
                }
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                font.features: { "tnum": 1 }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: root.vertical && clock.hasMeridiem
                text: visible ? clock.meridiem : ""
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
            Text {
                objectName: "taskbarDate"
                anchors.right: root.vertical ? undefined : parent.right
                anchors.horizontalCenter: root.vertical ? parent.horizontalCenter : undefined
                visible: Config.clockDate
                text: Qt.formatDate(clock.now, root.vertical ? "dd/MM" : "dd/MM/yyyy")
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
        }

        MouseArea {
            id: clockMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: mouse => {
                if (mouse.button === Qt.LeftButton) {
                    root.fromHere()
                    Shell.notificationCenterRequested()
                    return
                }
                root.openFromBar([
                    { text: qsTr("&Adjust date and time"), icon: "clock", enabled: System.available("datetime"), action: () => System.trigger("datetime") },
                    { text: qsTr("&Notification settings"), icon: "preferences-desktop-notification-symbolic", enabled: System.available("notification-settings"), action: () => System.trigger("notification-settings") }
                ], clock, mouse.x, mouse.y)
            }
        }
    }

    // The show-desktop strip is available at the far right of every taskbar.
    // It overlays the unused margin beside the clock without shifting layout.
    Item {
        id: showDesktop
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: 8
        height: 8 // on a vertical bar
        visible: Config.showDesktopCorner

        function showTooltip() {
            Menus.tooltip = {
                text: qsTr("Show desktop"),
                center: root.centerOnScreen(showDesktop),
                screen: root.screenName,
                owner: "show-desktop"
            }
        }
        function hideTooltip() {
            showDesktopTooltipDelay.stop()
            // Do not clear a tooltip belonging to another screen or component.
            if (Menus.tooltip && Menus.tooltip.owner === "show-desktop"
                    && Menus.tooltip.screen === root.screenName) {
                Menus.tooltip = null
            }
        }
        onVisibleChanged: if (!visible) hideTooltip()
        Component.onDestruction: hideTooltip()

        // The taskbar is a Window: hiding it does not change Item.visible.
        Connections {
            target: root
            function onVisibleChanged() {
                if (!root.visible) showDesktop.hideTooltip()
            }
        }
        Timer {
            id: showDesktopTooltipDelay
            interval: 500
            onTriggered: {
                if (root.visible && showDesktop.visible && showDesktopMouse.containsMouse) {
                    showDesktop.showTooltip()
                }
            }
        }
        Rectangle {
            x: root.vertical ? Math.round((parent.width - width) / 2) : parent.width - width - 2
            y: root.vertical ? parent.height - height - 2 : Math.round((parent.height - height) / 2)
            width: root.vertical ? 18 : 2
            height: root.vertical ? 2 : 18
            radius: 1
            color: Theme.textSecondary
            opacity: showDesktopMouse.containsMouse ? 0.9 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        }
        MouseArea {
            id: showDesktopMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            onContainsMouseChanged: {
                if (containsMouse) {
                    showDesktopTooltipDelay.restart()
                } else {
                    showDesktop.hideTooltip()
                }
            }
            onClicked: {
                showDesktop.hideTooltip()
                Tasks.toggleDesktop()
            }
        }
    }

    // A vertical bar (docs/taskbar.md §11.2): the tray's pieces stacked from
    // the bottom, the show-desktop strip last, each as wide as the bar (less a
    // floating pill's margin).
    Item {
        states: State {
            name: "vertical"
            when: root.vertical
            AnchorChanges {
                target: showDesktop
                anchors { right: undefined; top: undefined; bottom: parent.bottom; left: parent.left }
            }
            PropertyChanges { target: showDesktop; width: root.width; anchors.bottomMargin: root.inset }
            AnchorChanges {
                target: clock
                anchors { top: undefined; bottom: showDesktop.top; left: parent.left; right: parent.right }
            }
            PropertyChanges {
                target: clock
                anchors.bottomMargin: showDesktop.visible ? 0 : root.columnEnd
                anchors.leftMargin: root.inset
                anchors.rightMargin: root.inset
            }
            AnchorChanges {
                target: systemIcons
                anchors { right: parent.right; top: undefined; bottom: clock.top; left: parent.left }
            }
            PropertyChanges {
                target: systemIcons
                height: systemIcons.visible ? iconsRow.height + 16 : 0
                anchors.rightMargin: root.inset
                anchors.leftMargin: root.inset
                anchors.bottomMargin: 2
            }
            AnchorChanges {
                target: caffeine
                anchors { right: parent.right; top: undefined; bottom: systemIcons.top; left: parent.left }
            }
            PropertyChanges { target: caffeine; height: caffeine.visible ? 32 : 0; anchors.leftMargin: root.inset; anchors.rightMargin: root.inset }
            AnchorChanges {
                target: microphone
                anchors { right: parent.right; top: undefined; bottom: caffeine.top; left: parent.left }
            }
            PropertyChanges { target: microphone; height: microphone.visible ? 32 : 0; anchors.leftMargin: root.inset; anchors.rightMargin: root.inset }
            AnchorChanges {
                target: screenSharing
                anchors { right: parent.right; top: undefined; bottom: microphone.top; left: parent.left }
            }
            PropertyChanges { target: screenSharing; height: screenSharing.visible ? 32 : 0; anchors.leftMargin: root.inset; anchors.rightMargin: root.inset }
            AnchorChanges {
                target: tray
                anchors { right: undefined; top: undefined; bottom: screenSharing.top; horizontalCenter: parent.horizontalCenter }
            }
            PropertyChanges { target: tray; anchors.rightMargin: 0; anchors.bottomMargin: 4 }
            AnchorChanges {
                target: transferIndicator
                anchors { right: parent.right; top: undefined; bottom: tray.top; left: parent.left }
            }
            PropertyChanges {
                target: transferIndicator
                height: transferIndicator.visible ? 44 : 0
                anchors.rightMargin: root.inset
                anchors.leftMargin: root.inset
                anchors.bottomMargin: 4
            }
        }
    }
}
