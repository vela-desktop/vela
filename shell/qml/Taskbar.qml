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
    readonly property string screenName: screen ? screen.name : ""

    // It stays invisible until main.cpp has turned it into a layer-shell
    // panel. The compositor decides the width (anchored to the sides).
    visible: false
    width: 1280
    height: Theme.taskbarHeight
    color: "transparent"

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
    }

    // The whole taskbar above the blurred background (acrylic).
    function updateBlur() {
        Effects.setBlur(root, [Qt.rect(0, 0, width, height)])
    }
    onWidthChanged: updateBlur()
    onHeightChanged: updateBlur()

    // A button's window previews (TaskbarPreview.qml).
    Timer {
        id: previewDelay
        property var task: null
        interval: 450
        onTriggered: {
            if (!task || !task.hovered || task.windowCount === 0) return
            const p = task.mapToItem(null, task.width / 2, 0)
            Menus.preview = { key: task.key, appIds: Tasks.appIds(task.index), center: p.x, screen: root.screenName }
        }
    }

    // --- right-click menus (docs/renderer.md §14.4-14.6) ---

    // The taskbar is at the bottom of the output: from its coordinates to the
    // output's.
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        return Qt.point(p.x, Screen.height - root.height + p.y)
    }
    // Taskbar menus rise from its top edge.
    readonly property real menuBottom: Screen.height - root.height - 4

    function openAbove(entries, item, centered, options) {
        const p = screenPoint(item, centered ? item.width / 2 : 0, 0)
        root.openMenu(entries, p.x, menuBottom, Object.assign({ above: true, centered: centered }, options || {}))
    }
    // Taskbar menus open on its output; the panels it opens (Start, quick
    // settings...) go there too.
    function openMenu(entries, x, y, options) {
        Menus.panelScreen = root.screenName
        Menus.open(entries, x, y, Object.assign({ screen: root.screenName }, options || {}))
    }
    function fromHere() { Menus.panelScreen = root.screenName }

    // A button's jump list: pinned and recent files, the app's tasks, then the
    // app, pin/unpin, close.
    function jumpList(task) {
        const id = task.desktopId
        const entries = []
        if (id !== "") {
            const fileEntry = (file, pinned) => ({
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
                entries.push({ header: qsTr("Pinned") })
                pinned.forEach(f => entries.push(fileEntry(f, true)))
            }
            const recent = Jumps.recent(id, 10)
            if (recent.length > 0) {
                entries.push({ header: qsTr("Recent") })
                recent.forEach(f => entries.push(fileEntry(f, false)))
            }
            const actions = Apps.actions(id)
            if (actions.length > 0) {
                entries.push({ header: qsTr("Tasks") })
                actions.forEach(a => entries.push({ text: a.name.replace(/&/g, "&&"), icon: a.icon, action: () => Apps.launchAction(id, a.id) }))
            }
            if (entries.length > 0) {
                entries.push({ separator: true })
            }
            entries.push({ text: task.name.replace(/&/g, "&&"), icon: task.iconName, action: () => Apps.launchId(id) })
            entries.push(Tasks.isPinned(id)
                ? { text: qsTr("Unpin from taskbar"), icon: "window-unpin", action: () => Tasks.unpin(id) }
                : { text: qsTr("Pin to taskbar"), icon: "window-pin", action: () => Tasks.pin(id) })
        }
        if (task.windowCount > 0 && Config.endTask) {
            entries.push({ text: qsTr("End task"), icon: "process-stop", action: () => Tasks.endTask(task.index) })
        }
        if (task.windowCount > 0) {
            entries.push({
                text: task.windowCount > 1 ? qsTr("Close all windows") : qsTr("Close window"),
                icon: "window-close",
                action: () => Tasks.closeWindows(task.index)
            })
        }
        return entries
    }

    // Win+X: the Start button menu.
    function winXEntries() {
        const item = (text, icon, name) => ({ text: text, icon: icon, enabled: System.available(name), action: () => System.trigger(name) })
        const entries = [item(qsTr("Insta&lled apps"), "system-software-install", "installed-apps")]
        if (System.isLaptop()) {
            entries.push(item(qsTr("Mobility &Center"), "computer-laptop", "mobility"))
        }
        entries.push(
            item(qsTr("P&ower options"), "preferences-system-power-management", "power"),
            item(qsTr("E&vent viewer"), "text-x-log", "events"),
            item(qsTr("S&ystem"), "computer", "system"),
            item(qsTr("Device &manager"), "preferences-devices-tree", "devices"),
            item(qsTr("Net&work connections"), "preferences-system-network", "network"),
            item(qsTr("Dis&k management"), "drive-harddisk", "disks"),
            item(qsTr("Computer mana&gement"), "computer", "computer"),
            { separator: true },
            item(qsTr("&Terminal"), "utilities-terminal", "terminal"),
            item(qsTr("Terminal (&Admin)"), "utilities-terminal", "terminal-admin"),
            { separator: true },
            item(qsTr("Task manage&r"), "utilities-system-monitor", "task-manager"),
            item(qsTr("Setti&ngs"), "preferences-system", "settings"),
            item(qsTr("File &Explorer"), "system-file-manager", "files"),
            { text: qsTr("&Search"), icon: "search", action: () => { root.fromHere(); if (!Shell.startMenuOpen) Shell.toggleStartMenu() } },
            { text: qsTr("&Run"), icon: "system-run", action: () => { root.fromHere(); Shell.runRequested() } },
            { separator: true },
            { text: qsTr("Sh&ut down or sign out"), icon: "system-shutdown", children: [
                { text: qsTr("Sign &out"), icon: "system-log-out", action: () => Shell.logout() },
                { text: qsTr("S&leep"), icon: "system-suspend", enabled: Shell.canSuspend(), action: () => Shell.suspend() },
                { text: qsTr("Sh&ut down"), icon: "system-shutdown", action: () => Shell.powerOff() },
                { text: qsTr("&Restart"), icon: "system-reboot", action: () => Shell.reboot() }
            ] },
            { text: qsTr("&Desktop"), icon: "user-desktop", action: () => Tasks.toggleDesktop() }
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
        const p = screenPoint(startButton, 0, 0)
        root.openMenu(winXEntries(), p.x, menuBottom, { above: true, keyboard: keyboard })
    }

    // The menu of a notification area icon (the app decides it).
    Connections {
        target: Tray
        enabled: root.primary
        function onMenuReady(row, anchorX, entries) {
            const convert = list => list.map(e => e.separator ? { separator: true } : {
                text: e.label,
                icon: e.icon,
                enabled: e.enabled,
                checked: e.checkable ? e.checked : undefined,
                radio: e.radio,
                children: e.children.length > 0 ? convert(e.children) : undefined,
                action: () => Tray.activateMenuEntry(row, e.id)
            })
            root.openMenu(convert(entries), anchorX, root.menuBottom, { above: true, centered: true })
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.taskbar

        // Empty space: Task Manager and taskbar settings.
        MouseArea {
            id: emptyArea
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: mouse => {
                const p = root.screenPoint(emptyArea, mouse.x, 0)
                root.openMenu([
                    { text: qsTr("Task &manager"), icon: "utilities-system-monitor", enabled: System.available("task-manager"), action: () => System.trigger("task-manager") },
                    { text: qsTr("Taskbar &settings"), icon: "configure", enabled: System.available("taskbar-settings"), action: () => System.trigger("taskbar-settings") }
                ], p.x, root.menuBottom, { above: true })
            }
        }

        // A thin line of light on the top edge, like glass.
        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 1
            color: Theme.stroke
        }
    }

    Row {
        id: buttons
        // Centered by hand instead of with anchors: so when an app opens or
        // closes, the row slides to the new center instead of jumping. Or on
        // the left, like earlier versions of Windows.
        x: Config.taskbarAlignment === "left" ? 12 : Math.round((parent.width - width) / 2)
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        Behavior on x {
            enabled: Theme.motion > 0
            Eased {}
        }
        // The button of an app just opened comes in growing.
        add: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal }
            Eased { property: "scale"; from: 1 - 0.5 * Theme.motion; to: 1; duration: Theme.slow }
        }
        move: Transition {
            Eased { property: "x"; duration: Theme.normal * Theme.motion }
        }

        TaskbarButton {
            id: startButton
            active: Shell.startMenuOpen
            onClicked: { root.fromHere(); Shell.toggleStartMenu() }
            onRightClicked: root.openWinX(false)
            StartGlyph { anchors.centerIn: parent }
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

        Repeater {
            id: taskRepeater
            model: Tasks

            TaskbarButton {
                id: task
                draggable: true
                // Dropped further away: it moves by as many places as buttons
                // it passed.
                onDropped: dx => {
                    const to = Math.max(0, Math.min(taskRepeater.count - 1, index + Math.round(dx / (width + buttons.spacing))))
                    if (to !== index) Tasks.move(index, to)
                }
                onDraggingChanged: if (dragging) Menus.preview = null
                required property int index
                required property string key
                required property string name
                required property string iconName
                required property int windowCount
                required property bool windowActive
                required property string desktopId

                tooltip: name
                running: windowCount > 0
                active: windowActive
                onClicked: {
                    Menus.preview = null
                    Tasks.activate(index)
                }
                onMiddleClicked: Tasks.launchNew(index)
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
                    if (hovered && windowCount > 0) {
                        previewDelay.task = task
                        if (Menus.preview) {
                            previewDelay.triggered()
                        } else {
                            previewDelay.restart()
                        }
                    } else if (previewDelay.task === task) {
                        previewDelay.stop()
                    }
                }
                onRightClicked: shift => {
                    Menus.preview = null
                    if ((shift || Shell.shiftHeld()) && windowCount > 0) {
                        const state = Tasks.windowState(index)
                        root.openAbove(Menus.windowEntries(state.maximized, state.minimized, true,
                            action => Tasks.windowAction(task.index, action)), task, true)
                    } else {
                        root.openAbove(root.jumpList(task), task, true, { minWidth: 256, rebuild: () => root.jumpList(task) })
                    }
                }

                // The compositor flies minimized windows here.
                function reportGeometry() {
                    if (windowCount > 0) {
                        const p = mapToItem(null, 0, 0)
                        Tasks.setButtonGeometry(index, root, Qt.rect(p.x, p.y, width, height))
                    }
                }
                onXChanged: reportGeometry()
                onWindowCountChanged: reportGeometry()
                Connections {
                    target: buttons
                    function onXChanged() { task.reportGeometry() }
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

    // Session-wide transfer indicator. It stays visible even when the
    // originating File Explorer window is behind other windows.
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

        function openTransferMenu() {
            const entries = [{ header: qsTr("File transfers") }]
            for (const job of running) {
                const percent = job.total > 0 ? Math.round(100 * job.done / job.total) + "%" : qsTr("Preparing")
                entries.push({ text: job.title + " · " + percent, enabled: false })
                if (job.current !== "")
                    entries.push({ text: job.current, enabled: false })
                entries.push({ text: qsTr("Cancel transfer"), icon: "process-stop",
                               action: () => Transfers.cancel(job.owner, job.id) })
                entries.push({ separator: true })
            }
            root.openAbove(entries, transferIndicator, true, { minWidth: 310 })
        }
        Fill {
            anchors { fill: parent; topMargin: 8; bottomMargin: 8 }
            radius: Theme.radius
            color: transferMouse.pressed ? Theme.subtlePressed
                : transferMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
        }
        Icon {
            x: 7
            anchors.verticalCenter: parent.verticalCenter
            name: "document-save"
            size: 16
        }
        Text {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 6 }
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

    // Notification area: the apps' icons (Telegram, Discord, Steam...), left
    // of the clock like on Windows.
    Row {
        id: tray
        visible: root.primary
        anchors { right: microphone.left; top: parent.top; bottom: parent.bottom; rightMargin: 4 }

        Repeater {
            model: Tray

            delegate: Item {
                id: trayItem
                required property int index
                required property string icon
                required property string title
                required property bool attention
                width: icon !== "" ? 32 : 0
                height: tray.height
                visible: icon !== ""

                Fill {
                    anchors { fill: parent; topMargin: 8; bottomMargin: 8 }
                    radius: Theme.radius
                    color: trayMouse.pressed ? Theme.subtlePressed : trayMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                }
                Image {
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    source: trayItem.icon
                    sourceSize: Qt.size(width, height)
                    smooth: true
                    mipmap: true
                }
                // Asks for attention (unread messages): a dot.
                Rectangle {
                    visible: trayItem.attention
                    width: 6
                    height: 6
                    radius: 3
                    color: Theme.accent
                    anchors { right: parent.right; top: parent.top; rightMargin: 6; topMargin: 12 }
                }

                MouseArea {
                    id: trayMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                    onClicked: mouse => {
                        const center = trayItem.mapToItem(null, trayItem.width / 2, 0).x
                        if (mouse.button === Qt.RightButton) {
                            Tray.requestMenu(trayItem.index, center)
                        } else if (mouse.button === Qt.MiddleButton) {
                            Tray.secondaryActivate(trayItem.index)
                        } else {
                            Tray.activate(trayItem.index, center)
                        }
                    }
                    onWheel: wheel => Tray.scroll(trayItem.index, wheel.angleDelta.y)
                }
            }
        }
    }

    // The microphone in use (docs/audio.md §7.2), like Windows: who listens
    // in the tooltip, a click opens Settings > Sound.
    Item {
        id: microphone
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
            const p = microphone.mapToItem(null, microphone.width / 2, 0)
            Menus.tooltip = { text: microphone.tooltipText, center: p.x, screen: root.screenName, owner: "microphone" }
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
            anchors { fill: parent; topMargin: 8; bottomMargin: 8 }
            radius: Theme.radius
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
            const p = caffeine.mapToItem(null, caffeine.width / 2, 0)
            Menus.tooltip = { text: caffeine.tooltipText, center: p.x, screen: root.screenName, owner: "caffeine" }
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
                         { text: qsTr("&Power settings"), icon: "preferences-system-power-management",
                           enabled: System.available("power"), action: () => System.trigger("power") })
            root.openAbove(entries, caffeine, true, { minWidth: 220 })
        }

        Timer {
            id: caffeineTooltipDelay
            interval: 500
            onTriggered: caffeine.showTooltip()
        }
        Fill {
            anchors { fill: parent; topMargin: 8; bottomMargin: 8 }
            radius: Theme.radius
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
        visible: root.primary
        anchors { right: clock.left; top: parent.top; bottom: parent.bottom; rightMargin: 2 }
        width: visible ? iconsRow.width + 16 : 0

        Fill {
            anchors { fill: parent; topMargin: 4; bottomMargin: 4 }
            radius: Theme.radius
            color: systemMouse.pressed ? Theme.subtlePressed : systemMouse.containsMouse || Menus.quickSettingsOpen ? Theme.subtleHover : Theme.subtleRest
        }
        Row {
            id: iconsRow
            anchors.centerIn: parent
            spacing: 10
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
                const p = root.screenPoint(systemMouse, mouse.x, 0)
                const overVolume = volumeIcon.visible && mouse.x >= volumeIcon.x + iconsRow.x - 5
                    && mouse.x < volumeIcon.x + iconsRow.x + volumeIcon.width + 5
                root.openMenu(overVolume ? [
                    { text: qsTr("Open volume &mixer"), icon: "audio-volume-high", enabled: Audio.available, action: () => { root.fromHere(); Shell.soundOutputRequested() } },
                    { text: qsTr("&Sound settings"), icon: "preferences-desktop-sound", enabled: System.available("sound-settings"), action: () => System.trigger("sound-settings") }
                ] : [
                    { text: qsTr("&Troubleshoot network problems"), icon: "network-workgroup", enabled: false },
                    { text: qsTr("&Network and Internet settings"), icon: "preferences-system-network", enabled: System.available("network"), action: () => System.trigger("network") }
                ], p.x, root.menuBottom, { above: true })
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

    // Clock: time on top, date below.
    Item {
        id: clock
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom; rightMargin: 12 }
        width: clockColumn.implicitWidth + 16 + (Notifications.history.count > 0 || Notifications.doNotDisturb ? 22 : 0)

        property date now: new Date()

        Timer {
            interval: 1000
            running: true
            repeat: true
            onTriggered: clock.now = new Date()
        }

        Fill {
            anchors { fill: parent; topMargin: 4; bottomMargin: 4 }
            color: clockMouse.containsMouse || Menus.notificationCenterOpen ? Theme.subtleHover : Theme.subtleRest
        }
        // Notifications to read: a number next to the clock, like Windows 11
        // (with "Do not disturb" a muted bell).
        Rectangle {
            visible: Notifications.history.count > 0 || Notifications.doNotDisturb
            anchors { right: parent.right; rightMargin: -2; verticalCenter: parent.verticalCenter }
            width: 18
            height: 18
            radius: 9
            color: Notifications.doNotDisturb ? "transparent" : Theme.accent
            Text {
                anchors.centerIn: parent
                visible: !Notifications.doNotDisturb
                text: Math.min(Notifications.history.count, 9)
                color: "white"
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }
            Icon {
                anchors.centerIn: parent
                visible: Notifications.doNotDisturb
                name: "notifications-disabled"
                size: 14
            }
        }

        Column {
            id: clockColumn
            anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
            spacing: 1

            Text {
                anchors.right: parent.right
                text: Qt.formatTime(clock.now, "HH:mm")
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
            Text {
                anchors.right: parent.right
                text: Qt.formatDate(clock.now, "dd/MM/yyyy")
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
                const p = root.screenPoint(clock, mouse.x, 0)
                root.openMenu([
                    { text: qsTr("&Adjust date and time"), icon: "preferences-system-time", enabled: System.available("datetime"), action: () => System.trigger("datetime") },
                    { text: qsTr("&Notification settings"), icon: "preferences-desktop-notification", enabled: System.available("notification-settings"), action: () => System.trigger("notification-settings") }
                ], p.x, root.menuBottom, { above: true })
            }
        }
    }

    // The show-desktop strip is available at the far right of every taskbar.
    // It overlays the unused margin beside the clock without shifting layout.
    Item {
        id: showDesktop
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: 8
        visible: Config.showDesktopCorner

        function showTooltip() {
            const p = showDesktop.mapToItem(null, showDesktop.width / 2, 0)
            Menus.tooltip = {
                text: qsTr("Show desktop"),
                center: p.x,
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
            anchors { right: parent.right; rightMargin: 2; verticalCenter: parent.verticalCenter }
            width: 2
            height: 18
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
}
