// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Effects
import Vela.Controls

// The desktop wallpaper: a layer-shell surface below everything, even below
// the taskbar, one per output (see screenwindows.cpp). The compositor decides
// the size (anchored to all four sides). The main output has the desktop
// icons: the files of the Desktop folder and the Recycle Bin, like in Windows
// 11.
Window {
    id: root
    objectName: "wallpaper"

    visible: false
    width: Screen.width
    height: Screen.height
    color: Theme.desktop // while the image is being prepared

    property bool primary: false // set by screenwindows.cpp
    // Diagnostics only; enabled by VELA_DEBUG_SELECTION in setupWallpaper.
    // Normal desktop selection geometry is intentionally unchanged.
    property bool selectionTraceEnabled: false

    Image {
        anchors.fill: parent
        // Drawn already at the output's exact size, in real pixels: sourceSize
        // is in logical units, and Qt multiplies it by the window's scale
        // (fractional too) itself. Screen.devicePixelRatio instead is the
        // output's integer scale (2 at 125%): using it here asked for a
        // double-size image, then halved without filtering.
        sourceSize: Qt.size(width, height)
        source: width > 0 && height > 0
            ? "image://wallpaper/" + encodeURIComponent(Config.wallpaper)
            : ""
        asynchronous: true
        cache: false
        smooth: false // it's already the right size

        opacity: status === Image.Ready ? 1 : 0
        Behavior on opacity { Eased { duration: Theme.slow } }
    }

    // --- menus (docs/renderer.md §14.9) ---

    property point menuPoint // where the last menu opened: "Show more options" opens there

    function openMenu(entries, x, y, keyboard) {
        menuPoint = Qt.point(x, y)
        Menus.open(entries, x, y, { screen: root.screen ? root.screen.name : "", keyboard: !!keyboard })
    }

    // The KDE service menu entries for these files, in their submenus.
    function serviceEntries(paths) {
        const out = []
        const submenus = {}
        for (const a of ServiceMenus.actionsFor(paths)) {
            const entry = { text: a.text, icon: a.icon, action: () => ServiceMenus.run(a.id, paths) }
            if (a.submenu) {
                if (!submenus[a.submenu]) {
                    submenus[a.submenu] = { text: a.submenu, children: [] }
                    out.push(submenus[a.submenu])
                }
                submenus[a.submenu].children.push(entry)
            } else {
                out.push(entry)
            }
        }
        return out
    }

    // "Show more options" (Shift+F10, Shift+right click): the full menu, with
    // the entries apps added (KDE service menus), like the classic Windows
    // menu.
    function showMoreOptions(paths) {
        const x = menuPoint.x
        const y = menuPoint.y
        Qt.callLater(() => openMenu(paths.length > 0 ? classicFileEntries(paths) : classicDesktopEntries(), x, y))
    }

    function classicDesktopEntries() {
        const modern = desktopEntries()
        const entries = [modern[0], modern[1], modern[2], { separator: true },
            { text: qsTr("&Paste"), enabled: Desktop.canPaste, action: () => Desktop.paste() },
            { text: qsTr("Paste &shortcut"), enabled: Desktop.canPaste, action: () => { FileActions.pasteLinks(Desktop.directory); Desktop.refresh() } }]
        if (Desktop.undoText !== "") {
            entries.push({ text: qsTr("&Undo ") + Desktop.undoText, shortcut: "Ctrl+Z", action: () => Desktop.undo() })
        }
        const services = serviceEntries([Desktop.directory])
        entries.push({ separator: true })
        if (services.length > 0) {
            entries.push(...services, { separator: true })
        }
        entries.push(
            modern.find(e => e.text === qsTr("&New")),
            { separator: true },
            modern.find(e => e.text === qsTr("&Display settings")),
            modern.find(e => e.text === qsTr("&Personalize")))
        return entries
    }

    function classicFileEntries(paths) {
        const items = paths.map(p => icons.itemFor(p)).filter(i => i && !i.isTrash)
        if (items.length === 0) {
            return fileEntries(paths)
        }
        const files = items.map(i => i.path)
        const single = files.length === 1 ? items[0] : null
        const modern = fileEntries(files)
        const entries = [{ text: qsTr("&Open"), action: () => Desktop.openOnScreen(files, root.screen ? root.screen.name : Shell.primaryScreen) }]
        const openWith = modern.find(e => e.text === qsTr("Open &with"))
        if (openWith) {
            entries.push(openWith)
        }
        const services = serviceEntries(files)
        if (services.length > 0) {
            entries.push({ separator: true }, ...services)
        }
        entries.push(
            { separator: true },
            { text: qsTr("Cu&t"), action: () => Desktop.cut(files) },
            { text: qsTr("&Copy"), action: () => Desktop.copy(files) },
            { separator: true },
            { text: qsTr("Create &shortcut"), action: () => { FileActions.createLinks(files, Desktop.directory); Desktop.refresh() } },
            { text: qsTr("&Delete"), action: () => Desktop.trash(files) },
            { text: qsTr("Re&name"), enabled: !!single, action: () => icons.startRename(single.path) },
            { separator: true },
            { text: qsTr("P&roperties"), action: () => Menus.showProperties(files, root.screen ? root.screen.name : Shell.primaryScreen) })
        return entries
    }

    // The background: View, Sort by, Refresh, Undo, New...
    function desktopEntries() {
        const system = (text, icon, name) => ({ text: text, icon: icon, enabled: System.available(name), action: () => System.trigger(name) })
        const size = (text, value, shortcut) => ({ text: text, radio: true, checked: Desktop.iconSize === value, shortcut: shortcut, action: () => Desktop.iconSize = value })
        const sort = (text, value) => ({ text: text, radio: true, checked: Desktop.sortMode === value, action: () => Desktop.sortBy(value) })
        const newEntries = [
            { text: qsTr("&Folder"), icon: "folder-new", action: () => Desktop.createFolder() },
            { text: qsTr("&Shortcut"), icon: "insert-link", action: () => Menus.newShortcut(Desktop.directory, root.screen ? root.screen.name : Shell.primaryScreen) },
            { separator: true },
            { text: qsTr("&Text Document"), icon: "text-plain", action: () => Desktop.createFile("") }
        ]
        Desktop.templates().forEach(t => newEntries.push({ text: t.name.replace(/&/g, "&&"), icon: t.icon, action: () => Desktop.createFile(t.path) }))
        const entries = [
            { text: qsTr("&View"), icon: "view-list-icons", children: [
                size(qsTr("&Large icons"), 0, qsTr("Ctrl+Shift+2")),
                size(qsTr("&Medium icons"), 1, qsTr("Ctrl+Shift+3")),
                size(qsTr("&Small icons"), 2, qsTr("Ctrl+Shift+4")),
                { separator: true },
                { text: qsTr("&Auto arrange icons"), checked: Desktop.autoArrange, action: () => Desktop.autoArrange = !Desktop.autoArrange },
                { text: qsTr("Al&ign icons to grid"), checked: Desktop.alignToGrid, action: () => Desktop.alignToGrid = !Desktop.alignToGrid },
                { separator: true },
                { text: qsTr("Show desktop &icons"), checked: Desktop.showIcons, action: () => Desktop.showIcons = !Desktop.showIcons }
            ] },
            { text: qsTr("S&ort by"), icon: "view-sort", children: [
                sort(qsTr("&Name"), 0), sort(qsTr("&Size"), 1), sort(qsTr("Item &type"), 2), sort(qsTr("Date &modified"), 3)
            ] },
            { text: qsTr("R&efresh"), icon: "view-refresh", action: () => Desktop.refresh() }
        ]
        if (Desktop.undoText !== "") {
            entries.push({ text: qsTr("&Undo ") + Desktop.undoText, icon: "edit-undo", shortcut: "Ctrl+Z", action: () => Desktop.undo() })
        }
        if (Desktop.canPaste) {
            entries.push({ text: qsTr("&Paste"), icon: "edit-paste", shortcut: "Ctrl+V", action: () => Desktop.paste() })
        }
        entries.push(
            { separator: true },
            { text: qsTr("&New"), icon: "list-add", children: newEntries },
            { separator: true },
            system(qsTr("&Display settings"), "preferences-desktop-display", "display"),
            system(qsTr("&Personalize"), "preferences-desktop-wallpaper", "personalize"),
            { separator: true },
            { text: qsTr("Open in &Terminal"), icon: "utilities-terminal", action: () => System.openTerminal(Desktop.directory) },
            { separator: true },
            { text: qsTr("Show more options"), icon: "view-more-symbolic", shortcut: qsTr("Shift+F10"), action: () => root.showMoreOptions([]) }
        )
        return entries
    }

    // "Add to Favorites" (Explorer's Home): only for files.
    function favoriteEntry(files) {
        const all = files.length > 0 && files.every(p => FileActions.isFavorite(p))
        return all
            ? { text: qsTr("Remove from &Favorites"), icon: "starred-symbolic", action: () => files.forEach(p => FileActions.setFavorite(p, false)) }
            : { text: qsTr("Add to &Favorites"), icon: "starred-symbolic", enabled: files.length > 0, action: () => files.forEach(p => FileActions.setFavorite(p, true)) }
    }

    // One or more files: the icon row, then Open, Open with...
    function fileEntries(paths) {
        const items = paths.map(p => icons.itemFor(p)).filter(i => i)
        if (items.length === 0) {
            return []
        }
        const first = items[0]
        if (first.isTrash && items.length === 1) {
            return [
                { text: qsTr("&Open"), icon: "document-open", shortcut: qsTr("Enter"), action: () => Desktop.openOnScreen([first.path], root.screen ? root.screen.name : Shell.primaryScreen) },
                { text: qsTr("&Empty Recycle Bin"), icon: "trash-empty", enabled: !Desktop.trashEmpty(), action: () => icons.askEmptyTrash() },
                { separator: true },
                { text: qsTr("P&roperties"), icon: "document-properties", shortcut: qsTr("Alt+Enter"), enabled: false }
            ]
        }
        const files = items.filter(i => !i.isTrash).map(i => i.path)
        const single = files.length === 1 ? items.find(i => !i.isTrash) : null
        const entries = [{ iconRow: [
            { icon: "edit-cut", text: qsTr("Cut"), action: () => Desktop.cut(files) },
            { icon: "edit-copy", text: qsTr("Copy"), action: () => Desktop.copy(files) },
            { icon: "edit-rename", text: qsTr("Rename"), enabled: !!single, action: () => icons.startRename(single.path) },
            { icon: "document-share", text: qsTr("Share"), enabled: items.some(i => !i.isDir && !i.isTrash), action: () => Menus.share(items.filter(i => !i.isDir && !i.isTrash).map(i => i.path), root.screen ? root.screen.name : Shell.primaryScreen) },
            { icon: "edit-delete", text: qsTr("Delete"), action: () => Desktop.trash(files) }
        ] }]
        entries.push({ text: qsTr("&Open"), icon: "document-open", shortcut: qsTr("Enter"), action: () => Desktop.openOnScreen(files, root.screen ? root.screen.name : Shell.primaryScreen) })
        if (single && !single.isDir && !single.isApp) {
            const apps = Apps.appsForFile(single.path)
            const openWith = apps.map(a => ({ text: a.name.replace(/&/g, "&&"), icon: a.icon, action: () => Apps.launchWithFile(a.id, single.url) }))
            if (openWith.length > 0) {
                openWith.push({ separator: true })
            }
            openWith.push({ text: qsTr("Choose another app"), action: () => Menus.chooseApp(single.path, root.screen ? root.screen.name : Shell.primaryScreen) })
            entries.push({ text: qsTr("Open &with"), icon: "document-open", children: openWith })
        }
        if (single && single.isDir) {
            entries.push({ text: qsTr("Open in &Terminal"), icon: "utilities-terminal", action: () => System.openTerminal(single.path) })
        }
        if (single && single.isApp) {
            const id = Apps.idForDesktopFile(single.path)
            if (id !== "") {
                entries.push(Shell.startPins.indexOf(id) >= 0
                    ? { text: qsTr("Unpin from &Start"), icon: "window-unpin", action: () => Shell.unpinFromStart(id) }
                    : { text: qsTr("Pin to &Start"), icon: "window-pin", action: () => Shell.pinToStart(id) })
            }
        }
        entries.push(
            favoriteEntry(items.filter(i => !i.isTrash && !i.isDir).map(i => i.path)),
            { text: qsTr("Co&mpress to"), icon: "archive-insert", children: [
                { text: qsTr("&ZIP file"), icon: "application-zip", action: () => Desktop.compress(files, "zip") },
                { text: qsTr("&7z file"), icon: "application-x-7z-compressed", action: () => Desktop.compress(files, "7z") },
                { text: qsTr("&TAR file"), icon: "application-x-tar", action: () => Desktop.compress(files, "tar") }
            ] },
            { text: qsTr("Copy as &path"), icon: "edit-copy-path", shortcut: qsTr("Ctrl+Shift+C"), action: () => Desktop.copyAsPath(files) },
            { text: qsTr("P&roperties"), icon: "document-properties", shortcut: qsTr("Alt+Enter"), action: () => Menus.showProperties(files, root.screen ? root.screen.name : Shell.primaryScreen) },
            { separator: true },
            { text: qsTr("Show more options"), icon: "view-more-symbolic", shortcut: qsTr("Shift+F10"), action: () => root.showMoreOptions(files) }
        )
        return entries
    }

    // --- desktop icons ---

    Item {
        id: icons
        anchors { fill: parent; bottomMargin: Theme.taskbarHeight }
        focus: true

        // The model uses the sizes to lay out the icons.
        Binding { target: Desktop; property: "areaWidth"; value: icons.width; when: root.primary }
        Binding { target: Desktop; property: "areaHeight"; value: icons.height; when: root.primary }

        property var selection: ({}) // path -> true
        property string anchorPath: "" // for Shift+click
        property string renamingPath: ""
        property point dragPress // where the drag of desktop icons started
        property var dragUrls: []

        // Files dropped on the desktop: from an app they're copied or moved
        // here; the desktop's own icons move where they were dropped.
        DropArea {
            anchors.fill: parent
            enabled: root.primary
            onEntered: drag => {
                if (!drag.hasUrls) {
                    drag.accepted = false
                }
            }
            onDropped: drop => {
                if (drop.source && drop.source.isDesktopIcon) {
                    const dx = drop.x - icons.dragPress.x
                    const dy = drop.y - icons.dragPress.y
                    for (const path of icons.selectedPaths()) {
                        const it = icons.itemFor(path)
                        if (it) {
                            Desktop.moveTo(path, it.cellX + dx, it.cellY + dy)
                        }
                    }
                    drop.accept(Qt.MoveAction)
                    return
                }
                Desktop.drop(drop.urls.map(u => u.toString()), "", drop.x - Desktop.cellWidth / 2, drop.y - Desktop.iconPixels / 2)
                drop.accept(Qt.CopyAction)
            }
        }

        function selectedPaths() {
            return Object.keys(selection).filter(p => selection[p])
        }
        function isSelected(path) {
            return selection[path] === true
        }
        function selectOnly(path) {
            const s = {}
            if (path !== "") {
                s[path] = true
            }
            selection = s
            anchorPath = path
        }
        function toggle(path) {
            const s = Object.assign({}, selection)
            s[path] = !s[path]
            selection = s
            anchorPath = path
        }
        function selectRange(path) {
            const from = indexOf(anchorPath)
            const to = indexOf(path)
            if (from < 0 || to < 0) {
                selectOnly(path)
                return
            }
            const s = {}
            for (let i = Math.min(from, to); i <= Math.max(from, to); ++i) {
                s[repeater.itemAt(i).path] = true
            }
            selection = s
        }
        function indexOf(path) {
            for (let i = 0; i < repeater.count; ++i) {
                if (repeater.itemAt(i) && repeater.itemAt(i).path === path) {
                    return i
                }
            }
            return -1
        }
        function itemFor(path) {
            const i = indexOf(path)
            return i >= 0 ? repeater.itemAt(i) : null
        }
        function startRename(path) {
            if (path && path !== "trash:/") {
                selectOnly(path)
                renamingPath = path
            }
        }
        function askEmptyTrash() {
            Menus.confirm(qsTr("Delete multiple items"), qsTr("Permanently delete all the items in the Recycle Bin?"), qsTr("Yes"),
                () => Desktop.emptyTrash(), root.screen ? root.screen.name : Shell.primaryScreen)
        }
        // The menu for the selection or the background: the modern one with
        // the Menu key, the full one with Shift+F10 (like Windows 11).
        function keyboardMenu(classic) {
            const paths = selectedPaths()
            const item = paths.length > 0 ? itemFor(paths[0]) : null
            if (item) {
                root.openMenu(classic ? root.classicFileEntries(paths) : root.fileEntries(paths),
                    item.x + item.width / 2, item.y + item.height / 2, true)
            } else {
                root.openMenu(classic ? root.classicDesktopEntries() : root.desktopEntries(), 40, 40, true)
            }
        }
        // Arrows: the nearest icon in that direction.
        function moveSelection(dx, dy) {
            const current = itemFor(anchorPath)
            if (!current) {
                if (repeater.count > 0) {
                    selectOnly(repeater.itemAt(0).path)
                }
                return
            }
            let best = null
            let bestDistance = 1e9
            for (let i = 0; i < repeater.count; ++i) {
                const other = repeater.itemAt(i)
                const ox = other.x - current.x
                const oy = other.y - current.y
                if ((dx !== 0 && Math.sign(ox) !== dx) || (dy !== 0 && Math.sign(oy) !== dy)) {
                    continue
                }
                const distance = dx !== 0 ? Math.abs(ox) + 3 * Math.abs(oy) : Math.abs(oy) + 3 * Math.abs(ox)
                if (distance > 0 && distance < bestDistance) {
                    best = other
                    bestDistance = distance
                }
            }
            if (best) {
                selectOnly(best.path)
            }
        }

        Connections {
            target: Desktop
            function onCreated(path, rename) {
                icons.selectOnly(path)
                if (rename) {
                    icons.renamingPath = path
                }
            }
        }

        Keys.onPressed: event => {
            const ctrl = event.modifiers & Qt.ControlModifier
            const shift = event.modifiers & Qt.ShiftModifier
            const paths = selectedPaths()
            const files = paths.filter(p => p !== "trash:/")
            if (event.key === Qt.Key_Delete && files.length > 0) {
                Desktop.trash(files)
            } else if (event.key === Qt.Key_F2 && files.length === 1) {
                startRename(files[0])
            } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && paths.length > 0
                && !(event.modifiers & Qt.AltModifier)) {
                Desktop.openOnScreen(paths, root.screen ? root.screen.name : Shell.primaryScreen)
            } else if ((event.modifiers & Qt.AltModifier) && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                if (files.length > 0) {
                    Menus.showProperties(files, root.screen ? root.screen.name : Shell.primaryScreen)
                }
            } else if (event.key === Qt.Key_F5) {
                Desktop.refresh()
            } else if (ctrl && shift && event.key === Qt.Key_C) {
                Desktop.copyAsPath(files)
            } else if (ctrl && shift && event.key >= Qt.Key_2 && event.key <= Qt.Key_4) {
                Desktop.iconSize = event.key - Qt.Key_2
            } else if (ctrl && event.key === Qt.Key_A) {
                const s = {}
                for (let i = 0; i < repeater.count; ++i) {
                    s[repeater.itemAt(i).path] = true
                }
                selection = s
            } else if (ctrl && event.key === Qt.Key_C) {
                Desktop.copy(files)
            } else if (ctrl && event.key === Qt.Key_X) {
                Desktop.cut(files)
            } else if (ctrl && event.key === Qt.Key_V) {
                Desktop.paste()
            } else if (ctrl && event.key === Qt.Key_Z) {
                Desktop.undo()
            } else if (event.key === Qt.Key_Menu) {
                keyboardMenu(false)
            } else if (shift && event.key === Qt.Key_F10) {
                keyboardMenu(true)
            } else if (event.key === Qt.Key_Left) {
                moveSelection(-1, 0)
            } else if (event.key === Qt.Key_Right) {
                moveSelection(1, 0)
            } else if (event.key === Qt.Key_Up) {
                moveSelection(0, -1)
            } else if (event.key === Qt.Key_Down) {
                moveSelection(0, 1)
            } else {
                return
            }
            event.accepted = true
        }

        // The background: click to clear the selection, drag for the selection
        // box, right button for the desktop menu.
        MouseArea {
            id: background
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            property point origin
            property bool banding: false

            function traceSelection(phase, mouse) {
                if (!root.selectionTraceEnabled)
                    return
                const point = p => Number(p).toFixed(8)
                console.info("VELA_SELECTION qml phase=" + phase
                             + " screen=" + (root.screen ? root.screen.name : "none")
                             + " mouse=" + point(mouse.x) + "," + point(mouse.y)
                             + " origin=" + point(origin.x) + "," + point(origin.y)
                             + " band=" + [band.x, band.y, band.width, band.height].map(point).join(",")
                             + " window=" + point(root.width) + "x" + point(root.height)
                             + " screenDpr=" + (root.screen ? point(root.screen.devicePixelRatio) : "none"))
            }

            onPressed: mouse => {
                icons.forceActiveFocus()
                icons.renamingPath = ""
                if (mouse.button === Qt.RightButton) {
                    icons.selectOnly("")
                    return
                }
                if (!(mouse.modifiers & Qt.ControlModifier)) {
                    icons.selectOnly("")
                }
                origin = Qt.point(mouse.x, mouse.y)
                banding = true
                band.x = mouse.x
                band.y = mouse.y
                band.width = 0
                band.height = 0
                traceSelection("press", mouse)
            }
            onPositionChanged: mouse => {
                if (!banding) {
                    return
                }
                band.x = Math.min(origin.x, mouse.x)
                band.y = Math.min(origin.y, mouse.y)
                band.width = Math.abs(mouse.x - origin.x)
                band.height = Math.abs(mouse.y - origin.y)
                const s = {}
                for (let i = 0; i < repeater.count; ++i) {
                    const it = repeater.itemAt(i)
                    if (it.x < band.x + band.width && it.x + it.width > band.x
                        && it.y < band.y + band.height && it.y + it.height > band.y) {
                        s[it.path] = true
                    }
                }
                icons.selection = s
                traceSelection("move", mouse)
            }
            onReleased: mouse => {
                traceSelection("release", mouse)
                banding = false
            }
            onClicked: mouse => {
                if (mouse.button === Qt.RightButton) {
                    const classic = (mouse.modifiers & Qt.ShiftModifier) || Shell.shiftHeld()
                    root.openMenu(classic ? root.classicDesktopEntries() : root.desktopEntries(), mouse.x, mouse.y)
                }
            }
        }

        // The selection box, light blue like in Windows.
        Rectangle {
            id: band
            visible: background.banding && width > 2 && height > 2
            color: Qt.rgba(0.36, 0.55, 1.0, 0.25)
            border.width: 1
            border.color: Qt.rgba(0.36, 0.55, 1.0, 0.8)
            z: 2
        }

        Repeater {
            id: repeater
            model: root.primary ? Desktop : null

            delegate: Item {
                id: icon
                required property int index
                required property string name
                required property string path
                required property string url
                required property string iconName
                required property bool isDir
                required property bool isApp
                required property bool isTrash
                required property bool hasThumbnail
                required property double modified // seconds: a new thumbnail if the file changes
                required property real cellX
                required property real cellY

                readonly property bool selected: icons.isSelected(path)
                readonly property bool renaming: icons.renamingPath === path
                readonly property bool isDesktopIcon: true // to recognize them when they come back to the desktop

                Drag.active: iconDrag.active && icons.dragUrls.length > 0
                Drag.dragType: Drag.Automatic
                Drag.supportedActions: Qt.CopyAction | Qt.MoveAction | Qt.LinkAction
                Drag.proposedAction: Qt.MoveAction
                Drag.mimeData: ({ "text/uri-list": icons.dragUrls.join("\r\n") })
                Drag.imageSource: "image://fileicon/" + encodeURIComponent(icon.iconName)
                Drag.imageSourceSize: Qt.size(Desktop.iconPixels, Desktop.iconPixels)
                Drag.hotSpot: Qt.point(Desktop.iconPixels / 2, Desktop.iconPixels / 2)

                // A folder or the Recycle Bin: files can be dropped on it.
                DropArea {
                    id: iconDrop
                    anchors.fill: parent
                    enabled: (icon.isDir || icon.isTrash) && root.primary
                    onEntered: drag => {
                        // Not into itself (it's among those dragged).
                        if (!drag.hasUrls || (drag.source && drag.source.isDesktopIcon && icon.selected)) {
                            drag.accepted = false
                        }
                    }
                    onDropped: drop => {
                        Desktop.drop(drop.urls.map(u => u.toString()), icon.isTrash ? "trash:/" : icon.path, 0, 0)
                        drop.accept(Qt.CopyAction)
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 0
                    radius: 4
                    color: Qt.rgba(0.36, 0.55, 1.0, 0.30)
                    visible: iconDrop.containsDrag
                }

                width: Desktop.cellWidth
                height: Desktop.cellHeight
                x: cellX
                y: cellY
                z: selected ? 1 : 0

                // Highlight: on hover, and stronger if selected.
                Fill {
                    anchors.fill: content
                    anchors.margins: -2
                    radius: 4
                    color: icon.selected ? Qt.rgba(0.36, 0.55, 1.0, 0.30) : iconMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 1, 1, 0)
                    border.width: icon.selected ? 1 : 0
                    border.color: Qt.rgba(0.6, 0.75, 1.0, 0.6)
                }

                Column {
                    id: content
                    x: 2
                    y: 2
                    width: parent.width - 4
                    spacing: 4

                    // The file type's icon, or the thumbnail (images, and what
                    // other apps already put in the cache: videos, PDFs).
                    Item {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: Desktop.iconPixels
                        height: Desktop.iconPixels

                        Image {
                            anchors.fill: parent
                            visible: preview.status !== Image.Ready
                            source: "image://fileicon/" + encodeURIComponent(icon.iconName)
                            sourceSize: Qt.size(Desktop.iconPixels, Desktop.iconPixels)
                            smooth: true
                            mipmap: true
                        }
                        Image {
                            id: preview
                            anchors.fill: parent
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            source: icon.hasThumbnail
                                ? "image://filethumb/" + encodeURIComponent(icon.path) + "/" + icon.modified
                                : ""
                            sourceSize: Qt.size(Desktop.iconPixels, Desktop.iconPixels)
                            smooth: true
                            mipmap: true
                            // The thin outline of photos in Windows.
                            Rectangle {
                                anchors.centerIn: parent
                                width: preview.paintedWidth
                                height: preview.paintedHeight
                                visible: preview.status === Image.Ready
                                color: "transparent"
                                border.width: 1
                                border.color: Qt.rgba(1, 1, 1, 0.35)
                            }
                        }
                    }

                    // The name: white with a shadow, on two lines (all of it
                    // if selected).
                    Item {
                        width: parent.width
                        height: icon.renaming ? editor.height + 4 : label.height

                        Text {
                            id: label
                            visible: !icon.renaming
                            width: parent.width
                            text: icon.name
                            color: "white"
                            font.pixelSize: Theme.fontCaption
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            maximumLineCount: icon.selected ? 6 : 2
                            elide: Text.ElideRight
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                shadowEnabled: true
                                shadowColor: "black"
                                shadowOpacity: 0.9
                                shadowBlur: 0.25
                                shadowHorizontalOffset: 0
                                shadowVerticalOffset: 1
                            }
                        }

                        // Rename in place: the name without the extension is
                        // selected.
                        Rectangle {
                            visible: icon.renaming
                            width: parent.width
                            height: editor.height + 4
                            color: Theme.dialog
                            border.width: 1
                            border.color: Theme.accent
                            MenuTextField {
                                id: editor
                                x: 3
                                y: 2
                                width: parent.width - 6
                                font.pixelSize: Theme.fontCaption
                                horizontalAlignment: TextInput.AlignHCenter
                                wrapMode: TextInput.Wrap
                                mapToScreen: (x, y) => editor.mapToItem(null, x, y)
                                property bool done: false
                                function commit(save) {
                                    if (done) {
                                        return
                                    }
                                    done = true
                                    if (save && text !== icon.name) {
                                        Desktop.rename(icon.path, text)
                                    }
                                    icons.renamingPath = ""
                                    icons.forceActiveFocus()
                                }
                                Keys.onReturnPressed: commit(true)
                                Keys.onEnterPressed: commit(true)
                                Keys.onEscapePressed: commit(false)
                                onActiveFocusChanged: {
                                    if (!activeFocus && icon.renaming && !Menus.isOpen) {
                                        commit(true)
                                    }
                                }
                            }
                        }
                    }
                }

                onRenamingChanged: {
                    if (renaming) {
                        editor.done = false
                        editor.text = icon.path.split("/").pop()
                        const dot = icon.isDir ? -1 : editor.text.lastIndexOf(".")
                        editor.forceActiveFocus()
                        editor.select(0, dot > 0 ? dot : editor.text.length)
                    }
                }

                MouseArea {
                    id: iconMouse
                    anchors.fill: parent
                    enabled: !icon.renaming
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton

                    onPressed: mouse => {
                        icons.forceActiveFocus()
                        if (icons.renamingPath !== "") {
                            icons.renamingPath = ""
                        }
                        if (mouse.modifiers & Qt.ControlModifier) {
                            icons.toggle(icon.path)
                        } else if (mouse.modifiers & Qt.ShiftModifier) {
                            icons.selectRange(icon.path)
                        } else if (!icon.selected) {
                            icons.selectOnly(icon.path)
                        }
                    }
                    onReleased: mouse => {
                        if (mouse.button === Qt.LeftButton && !(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
                            icons.selectOnly(icon.path)
                        }
                    }
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton) {
                            const p = mapToItem(icons, mouse.x, mouse.y)
                            const classic = (mouse.modifiers & Qt.ShiftModifier) || Shell.shiftHeld()
                            const paths = icons.selectedPaths()
                            root.openMenu(classic ? root.classicFileEntries(paths) : root.fileEntries(paths), p.x, p.y)
                        }
                    }
                    onDoubleClicked: mouse => {
                        if (mouse.button === Qt.LeftButton) {
                            Desktop.openOnScreen([icon.path], root.screen ? root.screen.name : Shell.primaryScreen)
                        }
                    }

                    // Dragging the selected icons: a real drag between apps
                    // (files), which can end in Dolphin, in an email, in the
                    // Recycle Bin or back on the desktop (where the icons
                    // move).
                    DragHandler {
                        id: iconDrag
                        target: null
                        acceptedButtons: Qt.LeftButton
                        enabled: !icon.renaming
                        onActiveChanged: {
                            if (active) {
                                if (!icon.selected) {
                                    icons.selectOnly(icon.path)
                                }
                                const press = iconMouse.mapToItem(icons, centroid.pressPosition.x, centroid.pressPosition.y)
                                icons.dragPress = Qt.point(press.x, press.y)
                                icons.dragUrls = icons.selectedPaths().filter(p => p !== "trash:/")
                                    .map(p => icons.itemFor(p).url)
                            }
                        }
                    }
                }
            }
        }
    }
}
