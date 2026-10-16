// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes
import QtQuick.Effects
import Vela.Controls

// The Start menu: search at the top, all apps below, user at the bottom. It
// opens rising and fading; it closes on a click outside or with Esc.
Window {
    id: root

    visible: false
    width: 660
    // On short outputs it gets shorter: the search at the top must stay
    // visible.
    height: Math.min(732, Screen.height - Theme.taskbarHeight - 12)
    color: "transparent"

    property bool closing: false
    // Pinned tiles are in the GridView header rather than its model.
    // -1 means the keyboard selection is in the app model.
    property int selectedPinnedIndex: -1
    property string shortcutError: ""
    // Asked of logind only once: can the computer suspend?
    readonly property bool canSuspend: Shell.canSuspend()

    // The apps pinned to Start, installed, without duplicates (the same app as
    // a package and as a Flatpak).
    readonly property var pinned: {
        // entry(id) is not observable: depend explicitly on catalogue changes.
        Apps.catalogRevision
        const seen = {}
        const out = []
        for (const id of Shell.startPins) {
            const e = Apps.entry(id)
            if (e.name && !seen[e.name]) {
                seen[e.name] = true
                out.push({ id: id, name: e.name, iconName: e.iconName })
            }
        }
        return out
    }
    readonly property bool showPinned: search.text.length === 0 && pinned.length > 0

    // The Start menu is above the taskbar, centered or at the left: from its
    // coordinates to the output's, for right-click menus.
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        const left = Config.taskbarAlignment === "left" ? 12 : (Screen.width - root.width) / 2 // placeStartMenu
        return Qt.point(left + p.x, Screen.height - Theme.taskbarHeight - root.height + p.y)
    }

    function addToDesktop(id) {
        const path = Apps.desktopFile(id)
        const error = path === ""
            ? qsTr("Couldn't find this application's launcher.")
            : FileActions.createShortcut(Desktop.directory, path, "")
        if (error !== "") {
            shortcutError = error
            shortcutErrorTimer.restart()
            return
        }
        shortcutError = ""
        Desktop.refresh()
        root.close()
    }

    // An app's menu (docs/renderer.md §14.7): recent files and tasks at the
    // top, like the jump list; then the Start entries.
    function appMenu(id, pinnedTile) {
        const entries = []
        const recent = Jumps.recent(id, 5)
        if (recent.length > 0) {
            entries.push({ header: qsTr("Recent") })
            recent.forEach(f => entries.push({ text: f.name.replace(/&/g, "&&"), icon: f.icon, action: () => { Apps.launchWithFile(id, f.url); root.close() } }))
        }
        const actions = Apps.actions(id)
        if (actions.length > 0) {
            entries.push({ header: qsTr("Tasks") })
            actions.forEach(a => entries.push({ text: a.name.replace(/&/g, "&&"), icon: a.icon, action: () => { Apps.launchAction(id, a.id); root.close() } }))
        }
        if (entries.length > 0) {
            entries.push({ separator: true })
        }
        const inStart = Shell.startPins.indexOf(id) >= 0
        const taskbar = Tasks.isPinned(id)
            ? { text: qsTr("Unpin from &taskbar"), icon: "window-unpin", action: () => Tasks.unpin(id) }
            : { text: qsTr("Pin to &taskbar"), icon: "window-pin", action: () => Tasks.pin(id) }
        const desktop = { text: qsTr("Add to &desktop"), icon: "user-desktop", action: () => root.addToDesktop(id) }
        const folder = { text: qsTr("Open file &location"), icon: "document-open-folder", action: () => { System.showInFolder(Apps.desktopFile(id)); root.close() } }
        const uninstall = { text: qsTr("&Uninstall"), icon: "edit-delete", enabled: System.canUninstall(Apps.desktopFile(id)), action: () => { System.uninstall(Apps.desktopFile(id)); root.close() } }
        if (pinnedTile) {
            entries.push(
                { text: qsTr("&Unpin from Start"), icon: "window-unpin", action: () => Shell.unpinFromStart(id) },
                { text: qsTr("Move to &front"), icon: "go-top", enabled: Shell.startPins.indexOf(id) > 0, action: () => Shell.moveStartPinToFront(id) },
                taskbar, desktop, folder, uninstall)
        } else {
            entries.push(
                inStart
                    ? { text: qsTr("&Unpin from Start"), icon: "window-unpin", action: () => Shell.unpinFromStart(id) }
                    : { text: qsTr("&Pin to Start"), icon: "window-pin", action: () => Shell.pinToStart(id) },
                { text: qsTr("&More"), children: [taskbar, desktop, folder] },
                uninstall)
        }
        return entries
    }

    function openAppMenu(item, x, y, id, pinnedTile) {
        const p = screenPoint(item, x, y)
        Menus.open(appMenu(id, pinnedTile), p.x, p.y, { screen: root.screen ? root.screen.name : Shell.primaryScreen })
    }

    // An entry of the power menu.
    component PowerEntry: Item {
        id: entry
        property string icon // from the theme; empty: the symbol drawn inside
        property string label
        default property alias glyph: glyphSlot.data
        signal activated()
        width: parent ? parent.width : 0
        height: 36

        Rectangle {
            anchors.fill: parent
            radius: Theme.radius
            color: entryMouse.pressed ? Theme.subtlePressed : Theme.subtleHover
            opacity: entryMouse.containsMouse ? 1 : 0
        }
        Image {
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            visible: entry.icon !== ""
            source: entry.icon !== "" ? Theme.icons + entry.icon : ""
            sourceSize: Qt.size(width, height)
        }
        Item {
            id: glyphSlot
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
        }
        Text {
            x: 36
            anchors.verticalCenter: parent.verticalCenter
            text: entry.label
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        MouseArea {
            id: entryMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: entry.activated()
        }
    }

    // The blurred background under the panel (which rises while opening).
    function updateBlur() {
        Effects.setBlur(root, [Qt.rect(panel.x, Math.max(0, panel.y), panel.width, Math.max(0, Math.min(panel.height, root.height - panel.y)))])
    }
    Connections {
        target: panel
        function onYChanged() { root.updateBlur() }
        function onHeightChanged() { root.updateBlur() }
        function onWidthChanged() { root.updateBlur() }
    }

    function open() {
        // A second Super can reopen the menu while it is still closing.
        // The old animation must not finish by hiding the reopened window.
        closeAnimation.stop()
        closing = false
        powerButton.menuOpen = false
        Apps.query = ""
        search.text = ""
        grid.currentIndex = 0
        grid.keyNav = false
        selectedPinnedIndex = -1
        shortcutError = ""
        shortcutErrorTimer.stop()
        grid.positionViewAtBeginning()
        panel.opacity = 0
        panel.y = panel.restY + panel.slide
        Menus.placeOnTargetScreen(root) // the output of the taskbar it opens from
        visible = true
        Shell.startMenuOpen = true
        openAnimation.restart()
        search.forceActiveFocus()
    }

    function close() {
        if (!visible || closing) {
            return
        }
        closing = true
        Shell.startMenuOpen = false
        openAnimation.stop()
        closeAnimation.restart()
    }

    function launch(row) {
        if (row >= 0 && row < grid.count && Apps.launch(row)) {
            close()
        }
    }

    function launchSelection() {
        if (showPinned && selectedPinnedIndex >= 0 && selectedPinnedIndex < pinned.length) {
            if (Apps.launchId(pinned[selectedPinnedIndex].id)) {
                close()
            }
        } else {
            launch(grid.currentIndex)
        }
    }

    function selectPinned(index) {
        selectedPinnedIndex = index
        // A long pinned section may itself be taller than the visible grid.
        const top = Math.floor(index / 6) * grid.cellHeight
        if (grid.contentY > top) grid.contentY = top
        else if (grid.contentY + grid.height < top + grid.cellHeight)
            grid.contentY = top + grid.cellHeight - grid.height
    }

    function selectApp(index) {
        selectedPinnedIndex = -1
        grid.currentIndex = index
        grid.positionViewAtIndex(index, GridView.Contain)
    }

    // Navigate between the pinned header and the app model by column.
    // Left/right wrap inside the current section; up/down cross sections.
    function navigate(direction) {
        const pinCount = showPinned ? pinned.length : 0
        const appCount = grid.count
        if (pinCount === 0 && appCount === 0) return

        // No tile is highlighted before keyboard navigation starts, except
        // search results. The first arrow should select, not skip, tile zero.
        if (!grid.keyNav && search.text.length === 0) {
            grid.keyNav = true
            if (pinCount > 0) selectPinned(0)
            else selectApp(0)
            return
        }

        grid.keyNav = true
        const columns = 6
        if (selectedPinnedIndex >= 0 && pinCount > 0) {
            const index = selectedPinnedIndex
            if (direction === "left") selectPinned((index + pinCount - 1) % pinCount)
            else if (direction === "right") selectPinned((index + 1) % pinCount)
            else if (direction === "down") {
                if (index + columns < pinCount) selectPinned(index + columns)
                else if (appCount > 0) selectApp(Math.min(index % columns, appCount - 1))
                else selectPinned(index % columns)
            } else if (direction === "up") {
                if (index >= columns) selectPinned(index - columns)
                else if (appCount > 0) {
                    const lastRow = Math.floor((appCount - 1) / columns) * columns
                    selectApp(Math.min(lastRow + index % columns, appCount - 1))
                } else {
                    const lastRow = Math.floor((pinCount - 1) / columns) * columns
                    selectPinned(Math.min(lastRow + index % columns, pinCount - 1))
                }
            }
        } else if (appCount > 0) {
            const index = Math.max(0, grid.currentIndex)
            if (direction === "left") selectApp((index + appCount - 1) % appCount)
            else if (direction === "right") selectApp((index + 1) % appCount)
            else if (direction === "down") {
                if (index + columns < appCount) selectApp(index + columns)
                else if (pinCount > 0) selectPinned(Math.min(index % columns, pinCount - 1))
                else selectApp(index % columns)
            } else if (direction === "up") {
                if (index >= columns) selectApp(index - columns)
                else if (pinCount > 0) {
                    const lastRow = Math.floor((pinCount - 1) / columns) * columns
                    selectPinned(Math.min(lastRow + index % columns, pinCount - 1))
                } else {
                    const lastRow = Math.floor((appCount - 1) / columns) * columns
                    selectApp(Math.min(lastRow + index % columns, appCount - 1))
                }
            }
        } else {
            selectPinned(0)
        }
    }

    Timer {
        id: shortcutErrorTimer
        interval: 7000
        onTriggered: root.shortcutError = ""
    }

    Connections {
        target: Shell
        function onToggleStartRequested() {
            if (root.visible && !root.closing) {
                root.close()
            } else {
                root.open()
            }
        }
    }

    // A click on a window or elsewhere: the menu loses focus and closes, like
    // on Windows. A right-click menu opened from here takes the keyboard: the
    // Start menu stays open, and gets it back when the menu closes. Once the
    // menu closes, the keyboard comes back here; if it went elsewhere instead
    // (a new window, a click), this panel closes too.
    Connections {
        target: Menus
        function onIsOpenChanged() {
            if (!Menus.isOpen && root.visible) {
                focusCheck.restart()
            }
        }
    }
    Timer {
        id: focusCheck
        interval: 200
        onTriggered: {
            if (!root.active && root.visible && !Menus.isOpen) {
                root.close()
            }
        }
    }

    onActiveChanged: {
        if (!active && visible && !Menus.isOpen) {
            close()
        }
    }

    ParallelAnimation {
        id: openAnimation
        NumberAnimation {
            target: panel; property: "y"; to: panel.restY
            duration: Theme.slow
            easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate
        }
        NumberAnimation {
            target: panel; property: "opacity"; to: 1
            duration: Theme.normal
            easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate
        }
    }

    SequentialAnimation {
        id: closeAnimation
        ParallelAnimation {
            NumberAnimation {
                target: panel; property: "y"; to: panel.restY + panel.slide
                duration: Theme.normal
                easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.accelerate
            }
            NumberAnimation {
                target: panel; property: "opacity"; to: 0
                duration: Theme.normal
                easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.accelerate
            }
        }
        ScriptAction {
            script: {
                root.visible = false
                root.closing = false
            }
        }
    }

    // A soft shadow around the panel (not below: it's acrylic).
    PanelShadow {
        target: panel
        opacity: panel.opacity
        blur: 40
        offset: Qt.vector2d(0, 8)
        color: Qt.rgba(0, 0, 0, 0.5)
    }

    Rectangle {
        id: panel

        readonly property real restY: 10
        // How far it rises while opening. The part sticking out below the
        // window is cut at the taskbar's edge.
        readonly property real slide: 140
        x: 10
        y: restY
        width: root.width - 20
        height: root.height - 32 // 10 above, 22 below: 12 of gap from the taskbar
        radius: Theme.radiusOverlay
        color: Theme.surface
        border.width: 1
        border.color: Theme.stroke

        // --- search ---
        Rectangle {
            id: searchBox
            anchors { top: parent.top; left: parent.left; right: parent.right; margins: 24 }
            height: 38
            radius: height / 2
            color: Theme.control
            border.width: search.activeFocus ? 1 : 0
            border.color: Theme.accent

            Text {
                anchors { left: parent.left; leftMargin: 18; verticalCenter: parent.verticalCenter }
                visible: search.text.length === 0
                text: qsTr("Search apps")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }

            MenuTextField {
                id: search
                anchors { left: parent.left; right: parent.right; leftMargin: 18; rightMargin: 18; verticalCenter: parent.verticalCenter }
                mapToScreen: (x, y) => root.screenPoint(search, x, y)

                onTextChanged: {
                    Apps.query = text
                    grid.currentIndex = 0
                    root.selectedPinnedIndex = -1
                    grid.keyNav = false
                    grid.positionViewAtBeginning()
                }

                // The keyboard stays in the search; the arrows move the grid.
                Keys.onEscapePressed: root.close()
                Keys.onReturnPressed: root.launchSelection()
                Keys.onEnterPressed: root.launchSelection()
                Keys.onDownPressed: root.navigate("down")
                Keys.onUpPressed: root.navigate("up")
                Keys.onTabPressed: root.navigate("right")
                Keys.onBacktabPressed: root.navigate("left")
                // Left and Right edit text if the search isn't empty.
                Keys.onLeftPressed: event => {
                    if (text.length > 0) { event.accepted = false; return }
                    root.navigate("left")
                }
                Keys.onRightPressed: event => {
                    if (text.length > 0) { event.accepted = false; return }
                    root.navigate("right")
                }
            }
        }

        Text {
            id: sectionTitle
            anchors { top: searchBox.bottom; left: parent.left; topMargin: 20; leftMargin: 32 }
            text: search.text.length > 0 ? qsTr("Results") : root.showPinned ? qsTr("Pinned") : qsTr("All apps")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
        }

        // --- app grid ---
        GridView {
            id: grid
            anchors {
                top: sectionTitle.bottom; bottom: footer.top
                left: parent.left; right: parent.right
                topMargin: 10; leftMargin: 20; rightMargin: 20; bottomMargin: 8
            }
            clip: true
            model: Apps
            cellWidth: Math.floor(width / 6)
            cellHeight: 96
            keyNavigationWraps: true
            boundsBehavior: Flickable.StopAtBounds
            highlightFollowsCurrentItem: false
            // Search results are selected immediately; otherwise the first
            // navigation key makes the selection visible.
            property bool keyNav: false

            // With the arrows the selection can leave the view: we follow it.
            onCurrentIndexChanged: if (currentIndex >= 0 && root.selectedPinnedIndex < 0)
                positionViewAtIndex(currentIndex, GridView.Contain)

            delegate: AppTile {
                id: tile
                required property string appId
                width: grid.cellWidth
                height: grid.cellHeight
                current: root.selectedPinnedIndex < 0 && index === grid.currentIndex && (search.text.length > 0 || grid.keyNav)
                onActivated: root.launch(index)
                onContextRequested: (x, y) => root.openAppMenu(tile, x, y, tile.appId, false)
            }

            // Above all apps, those pinned to Start.
            header: Item {
                width: grid.width
                height: root.showPinned ? pinnedGrid.height + allTitle.height + 24 : 0
                visible: root.showPinned

                Grid {
                    id: pinnedGrid
                    columns: 6

                    Repeater {
                        model: root.pinned

                        delegate: AppTile {
                            id: pinnedTile
                            required property var modelData
                            width: grid.cellWidth
                            height: grid.cellHeight
                            name: modelData.name
                            iconName: modelData.iconName
                            comment: ""
                            current: grid.keyNav && root.showPinned && root.selectedPinnedIndex === index
                            onActivated: {
                                if (Apps.launchId(modelData.id)) {
                                    root.close()
                                }
                            }
                            onContextRequested: (x, y) => root.openAppMenu(pinnedTile, x, y, modelData.id, true)
                        }
                    }
                }
                Text {
                    id: allTitle
                    anchors { top: pinnedGrid.bottom; topMargin: 16; left: parent.left; leftMargin: 12 }
                    text: qsTr("All apps")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                }
            }

            Text {
                anchors.centerIn: parent
                visible: grid.count === 0
                text: qsTr("No apps found")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }
        }

        // A failed shortcut must not appear to have succeeded.
        Rectangle {
            visible: root.shortcutError !== ""
            z: 3
            anchors { left: parent.left; right: parent.right; bottom: footer.top
                leftMargin: 24; rightMargin: 24; bottomMargin: 8 }
            height: shortcutErrorText.implicitHeight + 24
            radius: Theme.radius
            color: Theme.popup
            border.width: 1
            border.color: Theme.stroke

            Text {
                id: shortcutErrorText
                width: parent.width - 24
                anchors.centerIn: parent
                text: root.shortcutError
                color: Theme.text
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
        }

        // --- footer: user ---
        Rectangle {
            id: footer
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 60
            color: "transparent"

            Rectangle {
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 1
                color: Theme.stroke
            }

            Row {
                anchors { left: parent.left; leftMargin: 32; verticalCenter: parent.verticalCenter }
                spacing: 12

                Rectangle {
                    width: 32
                    height: 32
                    radius: 16
                    color: Theme.accent
                    Text {
                        anchors.centerIn: parent
                        text: Shell.userInitial
                        color: "white"
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Shell.userName
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
            }

            // Power, where Windows has its button: sleep, sign out (closes
            // Vela, not the computer), restart, shut down.
            Item {
                id: powerButton
                anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                width: 40
                height: 40
                property bool menuOpen: false

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius
                    color: powerMouse.pressed ? Theme.subtlePressed : Theme.subtleHover
                    opacity: powerMouse.containsMouse || powerButton.menuOpen ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation { duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
                    }
                }

                // The power symbol: a circle open at the top and a bar.
                Shape {
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    preferredRendererType: Shape.CurveRenderer
                    scale: powerMouse.pressed ? 0.9 : 1
                    Behavior on scale {
                        NumberAnimation { duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
                    }
                    ShapePath {
                        strokeColor: Theme.text
                        strokeWidth: 1.5
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        PathAngleArc { centerX: 10; centerY: 11; radiusX: 6.5; radiusY: 6.5; startAngle: -50; sweepAngle: 280 }
                    }
                    ShapePath {
                        strokeColor: Theme.text
                        strokeWidth: 1.5
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        startX: 10; startY: 3
                        PathLine { x: 10; y: 10 }
                    }
                }

                MouseArea {
                    id: powerMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: powerButton.menuOpen = !powerButton.menuOpen
                }

                Rectangle {
                    id: powerMenu
                    anchors { right: parent.right; bottom: parent.top; bottomMargin: 8 }
                    width: 200
                    height: powerEntries.implicitHeight + 8
                    radius: Theme.radiusOverlay
                    color: Theme.popup
                    border.width: 1
                    border.color: Theme.stroke
                    opacity: powerButton.menuOpen ? 1 : 0
                    visible: opacity > 0
                    Behavior on opacity {
                        NumberAnimation { duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
                    }

                    Column {
                        id: powerEntries
                        x: 4
                        y: 4
                        width: parent.width - 8

                        PowerEntry {
                            icon: "system-suspend"
                            label: qsTr("Sleep")
                            visible: root.canSuspend
                            onActivated: { root.close(); Shell.suspend() }
                        }
                        PowerEntry {
                            label: qsTr("Sign out")
                            onActivated: Shell.logout()

                            // An open door and an arrow going out.
                            Shape {
                                anchors.fill: parent
                                preferredRendererType: Shape.CurveRenderer
                                ShapePath {
                                    strokeColor: Theme.text
                                    strokeWidth: 1.2
                                    fillColor: "transparent"
                                    capStyle: ShapePath.RoundCap
                                    joinStyle: ShapePath.RoundJoin
                                    startX: 8.5; startY: 2.5
                                    PathLine { x: 3; y: 2.5 }
                                    PathLine { x: 3; y: 13.5 }
                                    PathLine { x: 8.5; y: 13.5 }
                                }
                                ShapePath {
                                    strokeColor: Theme.text
                                    strokeWidth: 1.2
                                    fillColor: "transparent"
                                    capStyle: ShapePath.RoundCap
                                    joinStyle: ShapePath.RoundJoin
                                    startX: 6.5; startY: 8
                                    PathLine { x: 13.5; y: 8 }
                                    PathMove { x: 10.8; y: 5.3 }
                                    PathLine { x: 13.5; y: 8 }
                                    PathLine { x: 10.8; y: 10.7 }
                                }
                            }
                        }
                        PowerEntry {
                            icon: "system-reboot"
                            label: qsTr("Restart")
                            onActivated: Shell.reboot()
                        }
                        PowerEntry {
                            icon: "system-shutdown"
                            label: qsTr("Shut down")
                            onActivated: Shell.powerOff()
                        }
                    }
                }
            }
        }
    }
}
