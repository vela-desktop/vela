// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes
import QtQuick.Effects
import Vela.Controls
import "tiles.js" as Tiles

// The Start menu: search at the top, all apps below, user at the bottom. It
// opens rising from behind the taskbar and leaves the same way; it closes on
// a click outside or with Esc.
Window {
    id: root

    visible: false
    // The panel with room for its shadow around it (docs/visual-coherence.md
    // §6.9), cut where the window meets the taskbar, and at the output's edge
    // when the taskbar is aligned left (placeStartMenu sets `atLeft`): the
    // panel rests 12 units from both.
    property bool atLeft: false
    readonly property real room: shadow.reach
    readonly property real roomLeft: atLeft ? 12 : room
    width: roomLeft + 640 + room
    // On short outputs it gets shorter: the search at the top must stay
    // visible, the shadow above it is cut.
    readonly property real panelHeight: Math.min(700, Screen.height - Theme.taskbarHeight - 44)
    height: Math.min(room + panelHeight + 12, Screen.height - Theme.taskbarHeight)
    color: "transparent"
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

    // The keyboard's tile: a pinned one (`pinnedSelection`, its index) or the
    // grid's current app (-1). Shown once a key moves it, or at once among
    // search results.
    property int pinnedSelection: -1
    property bool selecting: false
    readonly property bool selectionShown: selecting || search.text.length > 0
    onPinnedChanged: pinnedSelection = Math.min(pinnedSelection, pinned.length - 1)

    // The Start menu is above the taskbar, centered or at the left: from its
    // coordinates to the output's, for right-click menus.
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        return Qt.point((atLeft ? 0 : (Screen.width - root.width) / 2) + p.x,
                        Screen.height - Theme.taskbarHeight - root.height + p.y)
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

    // A shortcut to the app's launcher on the desktop. Start closes once it
    // is there; a failure is said here, and Start stays.
    function addToDesktop(id) {
        const launcher = Apps.desktopFile(id)
        const error = launcher === "" ? qsTr("Couldn't find this application's launcher.")
            : FileActions.createShortcut(Desktop.directory, launcher, "")
        if (error !== "") {
            notice.show(error)
            return
        }
        Desktop.refresh()
        close()
    }

    // An entry of the power menu.
    // `icon.name` from the theme; without one, the symbol drawn inside.
    component PowerEntry: ListItem {
        id: entry
        default property alias glyph: glyphSlot.data
        width: parent ? parent.width : 0
        height: 36

        Icon {
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            name: entry.icon.name
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
            text: entry.text
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
    }

    function open() {
        // A second Super can reopen the menu while it is still closing: it
        // turns back from where it is, as it was.
        if (!motion.closing) {
            powerButton.menuOpen = false
            Apps.query = ""
            search.text = ""
            notice.hide()
            resetSelection()
            Menus.placeOnTargetScreen(root) // the output of the taskbar it opens from
        }
        visible = true
        Shell.startMenuOpen = true
        motion.open()
        search.forceActiveFocus()
        Effects.setBlur(root, [motion.visibleArea])
    }

    function close() {
        if (!motion.opened) {
            return
        }
        Shell.startMenuOpen = false
        motion.close()
    }

    function launch(row) {
        if (row >= 0 && row < grid.count && Apps.launch(row)) {
            close()
        }
    }

    // ---------------------------------------------------- keyboard selection --
    // The keys go through sections of tiles in rows of six: the pinned ones
    // (when shown), then all apps (tiles.js).

    function resetSelection() {
        pinnedSelection = -1
        selecting = false
        grid.currentIndex = 0
        grid.positionViewAtBeginning()
    }

    function sections() {
        return showPinned ? [pinned.length, grid.count] : [grid.count]
    }

    function select(section, index) {
        if (showPinned && section === 0) {
            pinnedSelection = index
            const tile = grid.headerItem ? grid.headerItem.tileAt(index) : null
            if (tile) {
                const top = tile.mapToItem(grid.contentItem, 0, 0).y
                if (top < grid.contentY) {
                    grid.contentY = top
                } else if (top + tile.height > grid.contentY + grid.height) {
                    grid.contentY = top + tile.height - grid.height
                }
            }
        } else {
            pinnedSelection = -1
            grid.currentIndex = index
            grid.positionViewAtIndex(index, GridView.Contain)
        }
    }

    function move(direction) {
        const counts = sections()
        if (counts.every(n => n === 0)) {
            return
        }
        // The first key shows where the selection is, on the first tile.
        if (!selectionShown) {
            selecting = true
            select(counts.findIndex(n => n > 0), 0)
            return
        }
        const section = pinnedSelection >= 0 ? 0 : counts.length - 1
        const index = pinnedSelection >= 0 ? pinnedSelection : Math.max(0, grid.currentIndex)
        selecting = true
        const to = Tiles.next(counts, section, index, direction, 6)
        select(to.section, to.index)
    }

    function launchSelection() {
        if (!selectionShown) {
            return
        }
        if (pinnedSelection >= 0) {
            if (Apps.launchId(pinned[pinnedSelection].id)) {
                close()
            }
        } else {
            launch(grid.currentIndex)
        }
    }

    Connections {
        target: Shell
        function onToggleStartRequested() {
            if (motion.opened) {
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

    ShadowRoom {
        panels: [panel]
        onOutside: root.close()
    }

    // It rises 140 units from behind the taskbar, where the window cuts it,
    // and leaves the same way.
    PanelMotion {
        id: motion
        x: root.roomLeft
        y: root.height - 12 - height
        width: 640
        height: root.panelHeight
        offset: Qt.point(0, 140)
        onClosed: root.visible = false
        onVisibleAreaChanged: Effects.setBlur(root, [visibleArea]) // the travel while it moves, the panel at rest

        Shadow {
            id: shadow
            target: panel
        }

        Rectangle {
            id: panel

            anchors.fill: parent
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
                        root.resetSelection()
                    }

                    // The keyboard stays in the search; the arrows move the
                    // selection, left and right only while there is no text
                    // to move through.
                    Keys.onEscapePressed: root.close()
                    Keys.onReturnPressed: root.launchSelection()
                    Keys.onEnterPressed: root.launchSelection()
                    Keys.onDownPressed: root.move("down")
                    Keys.onUpPressed: root.move("up")
                    Keys.onTabPressed: root.move("right")
                    Keys.onBacktabPressed: root.move("left")
                    Keys.onLeftPressed: event => text.length > 0 ? event.accepted = false : root.move("left")
                    Keys.onRightPressed: event => text.length > 0 ? event.accepted = false : root.move("right")
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

                delegate: AppTile {
                    id: tile
                    required property string appId
                    width: grid.cellWidth
                    height: grid.cellHeight
                    current: root.selectionShown && root.pinnedSelection < 0 && index === grid.currentIndex
                    onActivated: root.launch(index)
                    onContextRequested: (x, y) => root.openAppMenu(tile, x, y, tile.appId, false)
                }

                // Above all apps, those pinned to Start.
                header: Item {
                    width: grid.width
                    height: root.showPinned ? pinnedGrid.height + allTitle.height + 24 : 0
                    visible: root.showPinned
                    function tileAt(index) {
                        return pinnedTiles.itemAt(index)
                    }

                    Grid {
                        id: pinnedGrid
                        columns: 6

                        Repeater {
                            id: pinnedTiles
                            model: root.pinned

                            delegate: AppTile {
                                id: pinnedTile
                                required property var modelData
                                width: grid.cellWidth
                                height: grid.cellHeight
                                name: modelData.name
                                iconName: modelData.iconName
                                comment: ""
                                current: root.selectionShown && root.pinnedSelection === index
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

            // What went wrong, said where it was asked for (Add to desktop),
            // for a few seconds.
            Rectangle {
                id: notice
                property string text
                function show(message) {
                    text = message
                    hideTimer.restart()
                }
                function hide() {
                    text = ""
                    hideTimer.stop()
                }
                visible: text !== ""
                z: 1
                anchors { left: parent.left; right: parent.right; bottom: footer.top; leftMargin: 24; rightMargin: 24; bottomMargin: 8 }
                height: noticeText.implicitHeight + 24
                radius: Theme.radius
                color: Theme.popup
                border.width: 1
                border.color: Theme.stroke
                Text {
                    id: noticeText
                    anchors { fill: parent; margins: 12 }
                    text: notice.text
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.Wrap
                    verticalAlignment: Text.AlignVCenter
                }
                Timer {
                    id: hideTimer
                    interval: 7000
                    onTriggered: notice.text = ""
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
                Button {
                    id: powerButton
                    anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                    width: 40
                    height: 40
                    padding: 0
                    subtle: true
                    checked: menuOpen
                    property bool menuOpen: false
                    Accessible.name: qsTr("Power")
                    onClicked: menuOpen = !menuOpen

                    // The power symbol: a circle open at the top and a bar.
                    contentItem: Item {
                        Shape {
                            anchors.centerIn: parent
                            width: 20
                            height: 20
                            preferredRendererType: Shape.CurveRenderer
                            scale: powerButton.down ? 1 - 0.1 * Theme.motion : 1
                            Behavior on scale { Eased {} }
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
                        Behavior on opacity { Eased { duration: Theme.fast } }

                        Column {
                            id: powerEntries
                            x: 4
                            y: 4
                            width: parent.width - 8

                            PowerEntry {
                                icon.name: "system-suspend"
                                text: qsTr("Sleep")
                                visible: root.canSuspend
                                onClicked: { root.close(); Shell.suspend() }
                            }
                            PowerEntry {
                                text: qsTr("Sign out")
                                onClicked: Shell.logout()

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
                                icon.name: "system-reboot"
                                text: qsTr("Restart")
                                onClicked: Shell.reboot()
                            }
                            PowerEntry {
                                icon.name: "system-shutdown"
                                text: qsTr("Shut down")
                                onClicked: Shell.powerOff()
                            }
                        }
                    }
                }
            }
        }
    }
}
