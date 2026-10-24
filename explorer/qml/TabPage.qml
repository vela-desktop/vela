// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Files.Backend
import Vela.Controls

// An Explorer tab: the address bar, the command bar, the navigation pane and
// the content (Home, This PC or a folder), with its history. Selection,
// keyboard and right-click menus live here too (docs/renderer.md §14.9, the
// same as the desktop's).
Item {
    id: page
    property var menus // the window's MenuLayer
    property var win // the window: tabs, dialogs, view preferences
    property string location: "home:"

    readonly property bool isFolder: location.startsWith("/")
    readonly property bool isTrash: location === Places.trash
    readonly property bool writable: isFolder && !isTrash
    readonly property string title: location === "home:" ? qsTr("Home") : location === "thispc:" ? qsTr("This PC") : Places.displayName(location)
    readonly property string icon: location === "home:" ? "go-home" : location === "thispc:" ? "computer" : Places.iconFor(location)
    readonly property var model: isFolder ? folder : null
    readonly property bool showHidden: win.showHidden
    readonly property string viewMode: isFolder ? win.viewModeFor(location) : "details"
    property bool viewFocused: keys.activeFocus

    // --- history ---
    property var history: []
    property int historyIndex: -1
    readonly property bool canGoBack: historyIndex > 0
    readonly property bool canGoForward: historyIndex < history.length - 1
    readonly property bool canGoUp: isFolder && location !== "/"

    function navigate(loc, select, fromHistory) {
        if (loc === "") return
        if (!fromHistory) {
            const h = history.slice(0, historyIndex + 1)
            if (h[h.length - 1] !== loc) h.push(loc)
            history = h
            historyIndex = h.length - 1
        }
        renamingPath = ""
        location = loc
        if (isFolder) {
            folder.path = loc
            if (select) folder.selectPath(select)
        }
        focusView()
    }
    function back() { if (canGoBack) { historyIndex--; navigate(history[historyIndex], "", true) } }
    function forward() { if (canGoForward) { historyIndex++; navigate(history[historyIndex], "", true) } }
    function up() {
        if (!canGoUp) return
        const parent = location.substring(0, location.lastIndexOf("/")) || "/"
        navigate(parent, location)
    }
    function refresh() { if (isFolder) folder.reload(); else if (location === "thispc:") Places.refreshDrives() }
    // A drive not mounted yet: udisks mounts it (asking for the password if
    // needed), then we go there.
    property string mountingVolume: ""
    function openVolume(volume) {
        mountingVolume = volume
        Places.mount(volume)
    }
    Connections {
        target: Places
        function onMounted(volume, path, error) {
            if (volume !== page.mountingVolume) return
            page.mountingVolume = ""
            if (path !== "") page.navigate(path)
            else if (error !== "") page.showError(error)
        }
    }
    function openInNewTab(loc) { win.newTab(loc, "") }
    function search(text) { if (isFolder) folder.search = text }
    function focusView() { keys.forceActiveFocus() }
    function showError(message) { win.showError(message) }

    FolderModel {
        id: folder
        showHidden: page.showHidden
        onPathAppeared: row => {
            const path = folder.pathAt(row)
            contentLoader.item && contentLoader.item.ensureVisible && contentLoader.item.ensureVisible(row)
            if (path === page.renameWhenAppears) {
                page.renameWhenAppears = ""
                page.renamingPath = path
            }
        }
    }
    Connections {
        target: Ops
        // A file just created or pasted in this folder: it's selected (and
        // renamed).
        function onCreated(path, rename) {
            if (!page.isFolder || path.substring(0, path.lastIndexOf("/")) !== page.location) return
            if (rename) page.renameWhenAppears = path
            folder.selectPath(path)
        }
    }

    // --- mouse selection ---
    property int anchorRow: -1
    property int pendingSingle: -1
    function pressItem(row, button, modifiers) {
        focusView()
        if (button === Qt.RightButton || button === Qt.MiddleButton) {
            if (!folder.isSelected(row)) { folder.select(row); anchorRow = row }
            return
        }
        if (modifiers & Qt.ShiftModifier) {
            folder.selectRange(anchorRow >= 0 ? anchorRow : row, row, (modifiers & Qt.ControlModifier) !== 0)
        } else if (modifiers & Qt.ControlModifier) {
            folder.toggle(row)
            anchorRow = row
        } else if (!folder.isSelected(row)) {
            folder.select(row)
            anchorRow = row
        } else {
            folder.currentIndex = row
            pendingSingle = row // among several selected: on release only it stays (if not dragging)
        }
    }
    function releaseItem(row, button, modifiers) {
        if (pendingSingle === row && button === Qt.LeftButton && !(modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
            folder.select(row)
            anchorRow = row
        }
        pendingSingle = -1
    }
    function dragUrls(row) {
        folder.selectionVersion // reread when the selection changes
        return folder.isSelected(row) ? folder.selectedUrls() : [folder.urlAt(row)]
    }

    // --- opening ---
    function activate(row) {
        const path = folder.pathAt(row)
        if (folder.isDirAt(row)) {
            navigate(path)
        } else if (!isTrash) {
            Ops.open([path])
        }
    }
    function openSelection() {
        const paths = folder.selectedPaths()
        if (paths.length === 0) return
        const first = folder.indexOf(paths[0])
        if (paths.length === 1 && folder.isDirAt(first)) {
            navigate(paths[0])
            return
        }
        const files = paths.filter(p => !folder.isDirAt(folder.indexOf(p)))
        paths.filter(p => folder.isDirAt(folder.indexOf(p))).forEach(p => openInNewTab(p))
        if (files.length > 0 && !isTrash) Ops.open(files)
    }

    // --- operations ---
    property string renamingPath: ""
    property string renameWhenAppears: ""
    function copySelection() { if (folder.selectionCount > 0) Ops.copy(folder.selectedPaths()) }
    function cutSelection() { if (folder.selectionCount > 0 && writable) Ops.cut(folder.selectedPaths()) }
    function paste() { if (writable) Ops.paste(location) }
    function renameSelection() {
        if (writable && folder.selectionCount === 1) renamingPath = folder.selectedPaths()[0]
    }
    function finishRename(path, newName) {
        if (renamingPath !== path) return
        renamingPath = ""
        const result = Ops.rename(path, newName)
        if (result.startsWith("!")) {
            showError(result.substring(1))
        } else {
            folder.selectPath(path.substring(0, path.lastIndexOf("/") + 1) + result)
        }
        focusView()
    }
    function cancelRename() { renamingPath = ""; focusView() }
    function deleteSelection(permanent) {
        const paths = folder.selectedPaths()
        if (paths.length === 0 || (!writable && !isTrash)) return
        if (permanent || isTrash) {
            const what = paths.length === 1 ? "\"" + Places.displayName(paths[0]) + "\"" : qsTr("these ") + paths.length + qsTr(" items")
            win.confirm(qsTr("Permanently delete ") + what + "?", qsTr("This can't be undone."), qsTr("Delete"), () => Ops.deletePermanently(paths))
        } else {
            Ops.trash(paths)
        }
    }
    function askEmptyTrash() {
        win.confirm(qsTr("Empty the Recycle Bin?"), qsTr("All the items in the Recycle Bin will be permanently deleted."), qsTr("Empty"), () => Ops.emptyTrash())
    }
    function zoom(delta) {
        const modes = ["details", "small", "medium", "large"]
        const i = Math.max(0, Math.min(modes.length - 1, modes.indexOf(viewMode) + delta))
        win.setViewMode(location, modes[i])
    }

    // --- menus (docs/renderer.md §14.9) ---
    function openMenu(entries, x, y, keyboard) {
        menuPoint = Qt.point(x, y)
        menus.open(entries, x, y, { keyboard: !!keyboard })
    }
    property point menuPoint

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
    function showMoreOptions(paths) {
        const x = menuPoint.x
        const y = menuPoint.y
        Qt.callLater(() => openMenu(paths.length > 0 ? classicItemEntries(paths) : classicBackgroundEntries(), x, y))
    }

    function newEntries() {
        const entries = [
            { text: qsTr("&Folder"), icon: "folder-new", shortcut: qsTr("Ctrl+Shift+N"), action: () => Ops.createFolder(page.location) },
            { text: qsTr("&Shortcut"), icon: "insert-link", action: () => Ops.newShortcut(page.location) },
            { separator: true },
            { text: qsTr("&Text Document"), icon: "text-plain", action: () => Ops.createFile(page.location, "") }
        ]
        Ops.templates().forEach(t => entries.push({ text: t.name.replace(/&/g, "&&"), icon: t.icon, action: () => Ops.createFile(page.location, t.path) }))
        return entries
    }
    function sortEntries() {
        const sort = (text, column) => ({ text: text, radio: true, checked: folder.sortColumn === column, action: () => folder.sortBy(column, folder.sortDescending) })
        return [
            sort(qsTr("&Name"), 0), sort(qsTr("&Date modified"), 1), sort(qsTr("&Type"), 2), sort(qsTr("Si&ze"), 3),
            { separator: true },
            { text: qsTr("&Ascending"), radio: true, checked: !folder.sortDescending, action: () => folder.sortBy(folder.sortColumn, false) },
            { text: qsTr("D&escending"), radio: true, checked: folder.sortDescending, action: () => folder.sortBy(folder.sortColumn, true) }
        ]
    }
    function viewEntries() {
        const mode = (text, value, shortcut, icon) => ({ text: text, icon: icon, radio: true, checked: viewMode === value, shortcut: shortcut, action: () => win.setViewMode(page.location, value) })
        return [
            mode(qsTr("&Large icons"), "large", qsTr("Ctrl+Shift+2"), "view-list-icons"),
            mode(qsTr("&Medium icons"), "medium", qsTr("Ctrl+Shift+3"), "view-list-icons"),
            mode(qsTr("&Small icons"), "small", qsTr("Ctrl+Shift+4"), "view-list-text"),
            mode(qsTr("&Details"), "details", qsTr("Ctrl+Shift+6"), "view-list-details"),
            { separator: true },
            { text: qsTr("S&how"), icon: "view-visible", children: [
                { text: qsTr("&Preview pane"), checked: win.pane === "preview", shortcut: "Alt+P", action: () => win.togglePane("preview") },
                { text: qsTr("&Details pane"), checked: win.pane === "details", shortcut: qsTr("Alt+Shift+P"), action: () => win.togglePane("details") },
                { separator: true },
                { text: qsTr("&Hidden items"), checked: page.showHidden, shortcut: "Ctrl+H", action: () => win.showHidden = !win.showHidden }
            ] }
        ]
    }
    function moreEntries() {
        return [
            { text: qsTr("Select &all"), icon: "edit-select-all", shortcut: "Ctrl+A", action: () => folder.selectAll() },
            { text: qsTr("Select &none"), icon: "edit-select-none", action: () => folder.clearSelection() },
            { text: qsTr("&Invert selection"), icon: "edit-select-invert", action: () => folder.invertSelection() },
            { separator: true },
            { text: qsTr("Open in &Terminal"), icon: "utilities-terminal", enabled: page.isFolder, action: () => System.openTerminal(page.location) },
            { text: qsTr("P&roperties"), icon: "document-properties", shortcut: qsTr("Alt+Enter"),
              action: () => Ops.showProperties(folder.selectionCount > 0 ? folder.selectedPaths() : [page.location]) }
        ]
    }

    // The menu of one or more items.
    function itemMenu(row, x, y, keyboard) {
        if (!folder.isSelected(row)) folder.select(row)
        openMenu(itemEntries(folder.selectedPaths()), x, y, keyboard)
    }
    function itemEntries(paths) {
        if (isTrash) {
            return [
                { text: qsTr("&Restore"), icon: "edit-undo", action: () => Ops.restoreFromTrash(paths) },
                { separator: true },
                { text: qsTr("Cu&t"), icon: "edit-cut", enabled: false },
                { text: qsTr("&Delete permanently"), icon: "edit-delete", shortcut: qsTr("Del"), action: () => page.deleteSelection(true) },
                { separator: true },
                { text: qsTr("P&roperties"), icon: "document-properties", action: () => Ops.showProperties(paths) }
            ]
        }
        const single = paths.length === 1 ? paths[0] : ""
        const singleDir = single !== "" && folder.isDirAt(folder.indexOf(single))
        const files = paths.filter(p => !folder.isDirAt(folder.indexOf(p)))
        const entries = [{ iconRow: [
            { icon: "edit-cut", text: qsTr("Cut"), action: () => Ops.cut(paths) },
            { icon: "edit-copy", text: qsTr("Copy"), action: () => Ops.copy(paths) },
            { icon: "edit-rename", text: qsTr("Rename"), enabled: single !== "", action: () => page.renamingPath = single },
            { icon: "document-share", text: qsTr("Share"), enabled: files.length > 0, action: () => Ops.share(files) },
            { icon: "edit-delete", text: qsTr("Delete"), action: () => Ops.trash(paths) }
        ] }]
        entries.push({ text: qsTr("&Open"), icon: "document-open", shortcut: qsTr("Enter"), action: () => page.openSelection() })
        if (singleDir) {
            entries.push({ text: qsTr("Open in new &tab"), icon: "page-new", action: () => page.openInNewTab(single) })
            entries.push({ text: qsTr("Open in new &window"), icon: "window-new", action: () => Ops.newWindow(single) })
        }
        if (single !== "" && !singleDir) {
            const apps = Ops.appsFor(single)
            const openWith = apps.map(a => ({ text: a.name.replace(/&/g, "&&"), icon: a.icon, action: () => Ops.openWith(a.id, single) }))
            if (openWith.length > 0) openWith.push({ separator: true })
            openWith.push({ text: qsTr("Choose another app"), action: () => Ops.chooseApp(single) })
            entries.push({ text: qsTr("Open &with"), icon: "document-open", children: openWith })
        }
        // A shortcut: where the original is.
        if (single !== "" && Ops.isLink(single)) {
            const target = Ops.linkTarget(single)
            entries.push({ text: qsTr("Open file &location"), icon: "folder-open",
                action: () => page.navigate(target.substring(0, target.lastIndexOf("/")) || "/", target) })
        }
        if (files.length > 0 && files.length === paths.length) {
            const favorite = files.every(p => Places.isFavorite(p))
            entries.push(favorite
                ? { text: qsTr("Remove from &Favorites"), icon: "starred-symbolic", action: () => files.forEach(p => Places.setFavorite(p, false)) }
                : { text: qsTr("Add to &Favorites"), icon: "starred-symbolic", action: () => files.forEach(p => Places.setFavorite(p, true)) })
        }
        if (singleDir) {
            entries.push(Places.isPinned(single)
                ? { text: qsTr("Unpin from &Quick access"), icon: "window-unpin", action: () => Places.unpin(single) }
                : { text: qsTr("Pin to &Quick access"), icon: "window-pin", action: () => Places.pin(single) })
            entries.push({ text: qsTr("Open in &Terminal"), icon: "utilities-terminal", action: () => System.openTerminal(single) })
        }
        if (single !== "" && Ops.isArchive(single)) {
            entries.push({ text: qsTr("Extract &all..."), icon: "archive-extract", action: () => Ops.extractAll(single) })
        }
        entries.push(
            { text: qsTr("Co&mpress to"), icon: "archive-insert", children: [
                { text: qsTr("&ZIP file"), icon: "application-zip", action: () => Ops.compress(paths, "zip") },
                { text: qsTr("&7z file"), icon: "application-x-7z-compressed", action: () => Ops.compress(paths, "7z") },
                { text: qsTr("&TAR file"), icon: "application-x-tar", action: () => Ops.compress(paths, "tar") }
            ] },
            { text: qsTr("Copy as &path"), icon: "edit-copy-path", shortcut: qsTr("Ctrl+Shift+C"), action: () => Ops.copyAsPath(paths) },
            { text: qsTr("P&roperties"), icon: "document-properties", shortcut: qsTr("Alt+Enter"), action: () => Ops.showProperties(paths) },
            { separator: true },
            { text: qsTr("Show more options"), icon: "view-more-symbolic", shortcut: qsTr("Shift+F10"), action: () => page.showMoreOptions(paths) }
        )
        return entries
    }
    function classicItemEntries(paths) {
        const modern = itemEntries(paths)
        const entries = [{ text: qsTr("&Open"), action: () => page.openSelection() }]
        const openWith = modern.find(e => e.text === qsTr("Open &with"))
        if (openWith) entries.push(openWith)
        const services = serviceEntries(paths)
        if (services.length > 0) entries.push({ separator: true }, ...services)
        entries.push(
            { separator: true },
            { text: qsTr("Cu&t"), action: () => Ops.cut(paths) },
            { text: qsTr("&Copy"), action: () => Ops.copy(paths) },
            { separator: true },
            { text: qsTr("Create &shortcut"), action: () => Ops.createLinks(paths) },
            { text: qsTr("&Delete"), action: () => Ops.trash(paths) },
            { text: qsTr("Re&name"), enabled: paths.length === 1, action: () => page.renamingPath = paths[0] },
            { separator: true },
            { text: qsTr("P&roperties"), action: () => Ops.showProperties(paths) })
        return entries
    }

    // The menu of the folder's empty space.
    function backgroundMenu(x, y, keyboard) {
        if (!isFolder) return
        openMenu(backgroundEntries(), x, y, keyboard)
    }
    function backgroundEntries() {
        if (isTrash) {
            return [
                { text: qsTr("&View"), icon: "view-list-icons", children: viewEntries() },
                { text: qsTr("S&ort by"), icon: "view-sort", children: sortEntries() },
                { text: qsTr("R&efresh"), icon: "view-refresh", action: () => page.refresh() },
                { separator: true },
                { text: qsTr("&Empty Recycle Bin"), icon: "trash-empty", enabled: folder.count > 0, action: () => page.askEmptyTrash() }
            ]
        }
        const entries = [
            { text: qsTr("&View"), icon: "view-list-icons", children: viewEntries() },
            { text: qsTr("S&ort by"), icon: "view-sort", children: sortEntries() },
            { text: qsTr("R&efresh"), icon: "view-refresh", action: () => page.refresh() },
            { separator: true }
        ]
        if (Ops.undoText !== "") {
            entries.push({ text: qsTr("&Undo ") + Ops.undoText, icon: "edit-undo", shortcut: "Ctrl+Z", action: () => Ops.undo() })
        }
        entries.push(
            { text: qsTr("&Paste"), icon: "edit-paste", shortcut: "Ctrl+V", enabled: Ops.canPaste, action: () => page.paste() },
            { separator: true },
            { text: qsTr("&New"), icon: "list-add", children: newEntries() },
            { separator: true },
            { text: qsTr("P&roperties"), icon: "document-properties", action: () => Ops.showProperties([page.location]) },
            { text: qsTr("Open in &Terminal"), icon: "utilities-terminal", action: () => System.openTerminal(page.location) },
            { separator: true },
            { text: qsTr("Show more options"), icon: "view-more-symbolic", shortcut: qsTr("Shift+F10"), action: () => page.showMoreOptions([]) })
        return entries
    }
    function classicBackgroundEntries() {
        const entries = [
            { text: qsTr("&View"), children: viewEntries() },
            { text: qsTr("S&ort by"), children: sortEntries() },
            { text: qsTr("R&efresh"), action: () => page.refresh() },
            { separator: true },
            { text: qsTr("&Paste"), enabled: Ops.canPaste, action: () => page.paste() },
            { text: qsTr("Paste &shortcut"), enabled: Ops.canPaste, action: () => Ops.pasteLinks(page.location) }
        ]
        const services = serviceEntries([page.location])
        if (services.length > 0) entries.push({ separator: true }, ...services)
        entries.push({ separator: true }, { text: qsTr("&New"), children: newEntries() }, { separator: true },
            { text: qsTr("P&roperties"), action: () => Ops.showProperties([page.location]) })
        return entries
    }

    // --- keyboard ---
    property string typed: "" // the letters typed in a row: jump to the name
    Timer { id: typedReset; interval: 900; onTriggered: page.typed = "" }

    Item {
        id: keys
        focus: true
        Keys.onPressed: event => {
            const ctrl = event.modifiers & Qt.ControlModifier
            const shift = event.modifiers & Qt.ShiftModifier
            const alt = event.modifiers & Qt.AltModifier
            const view = contentLoader.item
            event.accepted = true
            // Quick preview: Space or Esc closes it; the arrows go on moving the selection.
            if (peek.visible && (event.key === Qt.Key_Space || event.key === Qt.Key_Escape)) peek.hide()
            else if (peek.visible && peek.kind === "pdf" && (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown))
                peek.pageBy(event.key === Qt.Key_PageUp ? -1 : 1)
            else if (event.key === Qt.Key_Space && !ctrl && !alt && page.isFolder && folder.selectionCount === 1)
                peek.show(folder.selectedPaths()[0])
            // Back and Forward (Alt+arrows): Main.qml, for the whole window.
            else if (alt && event.key === Qt.Key_Up) page.up()
            else if (event.key === Qt.Key_Backspace && !page.typed) page.back()
            else if (event.key === Qt.Key_F5 || (ctrl && event.key === Qt.Key_R)) page.refresh()
            else if (alt && event.key === Qt.Key_P) win.togglePane(shift ? "details" : "preview")
            else if (!page.isFolder) event.accepted = false
            else if (ctrl && shift && event.key === Qt.Key_N) Ops.createFolder(page.location)
            else if (ctrl && shift && event.key === Qt.Key_C) Ops.copyAsPath(folder.selectedPaths())
            else if (ctrl && shift && event.key === Qt.Key_2) win.setViewMode(page.location, "large")
            else if (ctrl && shift && event.key === Qt.Key_3) win.setViewMode(page.location, "medium")
            else if (ctrl && shift && event.key === Qt.Key_4) win.setViewMode(page.location, "small")
            else if (ctrl && shift && event.key === Qt.Key_6) win.setViewMode(page.location, "details")
            else if (ctrl && event.key === Qt.Key_A) folder.selectAll()
            else if (ctrl && event.key === Qt.Key_C) page.copySelection()
            else if (ctrl && event.key === Qt.Key_X) page.cutSelection()
            else if (ctrl && event.key === Qt.Key_V) page.paste()
            else if (ctrl && event.key === Qt.Key_Z) Ops.undo()
            else if (ctrl && event.key === Qt.Key_H) win.showHidden = !win.showHidden
            else if (event.key === Qt.Key_Delete) page.deleteSelection(shift)
            else if (event.key === Qt.Key_F2) page.renameSelection()
            else if (alt && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter))
                Ops.showProperties(folder.selectionCount > 0 ? folder.selectedPaths() : [page.location])
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) page.openSelection()
            else if (event.key === Qt.Key_Menu || (shift && event.key === Qt.Key_F10)) {
                const row = folder.currentIndex
                const p = page.mapToItem(null, page.width / 2, page.height / 2)
                if (shift && event.key === Qt.Key_F10) {
                    page.menuPoint = p
                    page.showMoreOptions(folder.selectedPaths())
                } else if (row >= 0 && folder.isSelected(row)) page.itemMenu(row, p.x, p.y, true)
                else page.backgroundMenu(p.x, p.y, true)
            }
            else if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                const row = event.key === Qt.Key_Home ? 0 : folder.count - 1
                page.moveTo(row, shift, ctrl)
            }
            else if ([Qt.Key_Up, Qt.Key_Down, Qt.Key_Left, Qt.Key_Right, Qt.Key_PageUp, Qt.Key_PageDown].indexOf(event.key) >= 0) {
                if (folder.count === 0) return
                if (folder.currentIndex < 0) { page.moveTo(0, false, false); return }
                if (view && view.step) page.moveTo(view.step(event.key), shift, ctrl)
            }
            else if (event.text.length === 1 && event.text.charCodeAt(0) > 32 && !ctrl && !alt) {
                // Typing a name jumps there, like Windows.
                page.typed += event.text
                typedReset.restart()
                const row = folder.find(page.typed, page.typed.length === 1 ? folder.currentIndex : folder.currentIndex - 1)
                if (row >= 0) page.moveTo(row, false, false)
            }
            else event.accepted = false
        }
    }
    // The peek follows the selection while it is open.
    Connections {
        target: folder
        function onSelectionVersionChanged() {
            if (!peek.visible) return
            if (folder.selectionCount === 1) peek.path = folder.selectedPaths()[0]
            else peek.hide()
        }
    }
    onLocationChanged: peek.hide()
    onRenamingPathChanged: if (renamingPath !== "") peek.hide()
    function moveTo(row, shift, ctrl) {
        if (row < 0 || row >= folder.count) return
        if (shift) folder.selectRange(anchorRow >= 0 ? anchorRow : folder.currentIndex, row, ctrl)
        else if (ctrl) folder.currentIndex = row
        else { folder.select(row); anchorRow = row }
        if (contentLoader.item && contentLoader.item.ensureVisible) contentLoader.item.ensureVisible(row)
    }

    // --- look ---
    AddressBar {
        id: addressBar
        tab: page
        anchors { left: parent.left; right: parent.right; top: parent.top }
    }
    CommandBar {
        id: commandBar
        tab: page
        anchors { left: parent.left; right: parent.right; top: addressBar.bottom }
    }
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: commandBar.bottom; bottom: parent.bottom }
        color: Theme.layer
        topLeftRadius: Theme.radiusOverlay
        topRightRadius: Theme.radiusOverlay
        border.width: 1
        border.color: Theme.layerStroke

        NavPane {
            id: nav
            anchors { left: parent.left; top: parent.top; bottom: statusBar.top }
            width: page.win.navWidth
            current: page.location
            onNavigate: location => page.navigate(location)
            onOpenInNewTab: location => page.openInNewTab(location)
            onOpenVolume: volume => page.openVolume(volume)
            onContextMenu: (entries, x, y) => page.openMenu(entries, x, y)
        }
        // The pane's border: drag to widen it.
        Rectangle {
            id: splitter
            anchors { left: nav.right; top: parent.top; bottom: statusBar.top }
            width: 1
            color: Theme.divider
            MouseArea {
                anchors { fill: parent; leftMargin: -4; rightMargin: -4 }
                cursorShape: Qt.SplitHCursor
                onPositionChanged: mouse => {
                    if (pressed) page.win.navWidth = Math.max(160, Math.min(420, mapToItem(nav.parent, mouse.x, 0).x))
                }
            }
        }

        Loader {
            id: contentLoader
            anchors { left: splitter.right; right: sidePane.visible ? paneSplitter.left : parent.right; top: parent.top; bottom: statusBar.top }
            sourceComponent: page.location === "home:" ? homeComponent
                : page.location === "thispc:" ? thisPcComponent
                : page.viewMode === "details" ? detailsComponent : iconComponent
        }
        // The preview or details pane, on the right (widened by dragging its
        // border).
        Rectangle {
            id: paneSplitter
            visible: sidePane.visible
            anchors { right: sidePane.left; top: parent.top; bottom: statusBar.top }
            width: 1
            color: Theme.divider
            MouseArea {
                anchors { fill: parent; leftMargin: -4; rightMargin: -4 }
                cursorShape: Qt.SplitHCursor
                onPositionChanged: mouse => {
                    if (pressed) {
                        const x = mapToItem(sidePane.parent, mouse.x, 0).x
                        page.win.paneWidth = Math.max(200, Math.min(sidePane.parent.width / 2, sidePane.parent.width - x))
                    }
                }
            }
        }
        PreviewPane {
            id: sidePane
            visible: page.win.pane !== "" && page.location !== "home:" && page.location !== "thispc:"
            anchors { right: parent.right; top: parent.top; bottom: statusBar.top }
            width: page.win.paneWidth
            tab: page
            mode: page.win.pane || "preview"
        }

        Component { id: homeComponent; HomePage { tab: page } }
        Component { id: thisPcComponent; ThisPcPage { tab: page } }
        Component { id: detailsComponent; DetailsView { tab: page } }
        Component { id: iconComponent; IconView { tab: page; mode: page.viewMode } }

        StatusBar {
            id: statusBar
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            model: page.model
            viewMode: page.viewMode
            message: page.location === "thispc:" ? Places.drives.length + qsTr(" drives") : ""
            onViewRequested: mode => page.win.setViewMode(page.location, mode)
        }
    }

    QuickPeek { id: peek; tab: page }

    Component.onCompleted: focusView()
}
