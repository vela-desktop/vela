// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton

import QtQuick
import Vela.Controls

// All of Vela's right-click menus go through here (docs/renderer.md §14):
// whoever wants a menu calls Menus.open(entries, x, y, options) and the
// ContextMenu window shows it. x and y are in output coordinates.
//
// An entry is a JS object:
//   { text: "&Open", icon: "document-open", shortcut: "Ctrl+O",
//     enabled: true, checked: true/false (only if checkable), radio: true,
//     action: function() {...}, children: [entries], context: [entries] }
//   { separator: true }       a separator line
//   { header: "Recent" }      a group title (jump list)
//   { ..., pin: { pinned: false, toggle: function() {...} } }  the pin
// "&" marks the keyboard letter (underlined when opened from the keyboard).
//
// Options: screen (output name), above (the menu rises from y), centered
// (x is the center), before (the menu ends at x: beside a taskbar on the
// right), keyboard (opened from the keyboard), minWidth, rebuild
// (a function rebuilding the entries, such as after pinning a file), onClosed.
QtObject {
    id: menus

    // Open: the panels it starts from (such as the Start menu) don't close
    // when they lose the keyboard to the menu.
    property bool isOpen: false
    // The panels on the right of the taskbar: one opens at a time.
    property bool quickSettingsOpen: false
    property bool notificationCenterOpen: false
    // Task View is open.
    property bool taskViewOpen: false
    // The snap layout just chosen ({window, zones: [[x0,y0,x1,y1]...]}): Snap
    // Assist offers the windows for its other zones.
    property var snapLayout: null
    // TaskbarPreview.qml uses the button, its center, and the opening mode.
    property var preview: null
    property var trayOverflow: null
    signal advancePreview()
    // A tooltip above the taskbar (TaskbarTooltip.qml): {text, center (x on
    // the output), screen}, or null.
    property var tooltip: null
    // The output of the taskbar a panel was opened from (Start, quick
    // settings, notification center, Task View): the panel goes there. Empty:
    // the display the user is working on (the focused window's, as the
    // compositor says), else the main output (Shell.panelScreen).
    property string panelScreen: ""
    function targetScreen() {
        const name = panelScreen !== "" ? panelScreen : Shell.panelScreen()
        panelScreen = ""
        return Qt.application.screens.find(s => s.name === name) || null
    }
    // Puts `window` (hidden) on the panel's output; that output.
    function placeOnTargetScreen(window) {
        const screen = targetScreen()
        if (screen) {
            Shell.placeOnScreen(window, screen.name)
        }
        return screen
    }
    // Where each output's taskbar has its clock and its system icons, while
    // they sit in a centered pill (a taskbar style's fit): { screen: { clock,
    // icons } }, x on the output. The notification center and quick settings
    // open above them, not at the output's right edge, whether clicked or
    // from the keyboard.
    property var trayAnchors: ({})
    function setTrayAnchors(screen, anchors) {
        const all = Object.assign({}, trayAnchors)
        if (anchors) {
            all[screen] = anchors
        } else {
            delete all[screen]
        }
        trayAnchors = all
    }
    // Puts a tray panel (its panel `width` wide at `x` in its window) on the
    // panel's output, above (or below) `button` ("clock", "icons") of a
    // centered pill there, else in the clock's corner (Styles.taskbar's
    // trayCorner). Returns the window's left edge on the output, or -1 in the
    // corner (for the panel's screenPoint()).
    function placeTrayPanel(window, button, x, width) {
        const screen = placeOnTargetScreen(window)
        const anchors = screen ? trayAnchors[screen.name] : undefined
        if (!anchors || anchors[button] === undefined || Styles.taskbar.vertical) {
            Shell.placeAnchored(window, Styles.taskbar.trayCorner, 0, 0)
            return -1
        }
        const left = Math.round(Math.max(0, Math.min(screen.width - window.width, anchors[button] - x - width / 2)))
        Shell.placeAnchored(window, Styles.taskbar.edgeAnchor | 4, left, 0)
        return left
    }
    // A tray panel's top-left on the output: in its corner, or `left` along
    // a centered pill (placeTrayPanel's answer).
    function trayPanelOrigin(window, left) {
        const bar = Styles.taskbar
        const edges = left >= 0 ? bar.edgeAnchor | 4 : bar.trayCorner
        return bar.origin(edges, window.width, window.height, window.screen.width, window.screen.height, Math.max(0, left), 0)
    }
    // A window opening from a taskbar button (its previews, tooltip, the
    // tray's drawer): against the bar on `screen`, centered on `center` (along
    // the bar, on the output), `length` long along it (its size by
    // default). Returns its start along the bar.
    function placeBesideBar(window, center, screen, size) {
        const bar = Styles.taskbar
        const length = size !== undefined ? size : bar.vertical ? window.height : window.width
        const total = bar.vertical ? (screen ? screen.height : window.screen.height) : (screen ? screen.width : window.screen.width)
        const start = Math.round(Math.max(0, Math.min(total - length, center - length / 2)))
        // Against the bar's edge; along it from the left of a row, the top
        // of a column.
        Shell.placeAnchored(window, bar.edgeAnchor | (bar.vertical ? 1 : 4), bar.vertical ? 0 : start, bar.vertical ? start : 0)
        return start
    }
    property bool previewHovered: false
    property bool previewButtonHovered: false

    signal openRequested(var entries, real x, real y, var options)
    signal closeRequested()
    // A Yes/No question (such as "Empty Recycle Bin"): ConfirmDialog.qml.
    signal confirmRequested(string title, string text, string yesText, var action, string screen)

    function confirm(title, text, yesText, action, screen) {
        confirmRequested(title, text, yesText, action, screen || Shell.primaryScreen)
    }

    // The Properties window (PropertiesDialog.qml) for these files.
    signal propertiesRequested(var paths, string screen)
    function showProperties(paths, screen) {
        propertiesRequested(paths, screen || Shell.primaryScreen)
    }

    // Share, "Choose another app", New > Shortcut (FileDialogs.qml).
    signal shareRequested(var paths, string screen)
    signal openWithRequested(string path, string screen)
    signal newShortcutRequested(string folder, string screen)
    function share(paths, screen) { shareRequested(paths, screen || Shell.primaryScreen) }
    function chooseApp(path, screen) { openWithRequested(path, screen || Shell.primaryScreen) }
    function newShortcut(folder, screen) { newShortcutRequested(folder, screen || Shell.primaryScreen) }

    function open(entries, x, y, options) {
        const request = options || {}
        isOpen = true
        openRequested(entries, x, y, request)
    }

    function close() {
        closeRequested()
    }

    // The menu entries of a text field, like in Windows.
    function textEntries(input) {
        const selected = input.selectedText.length > 0
        const editable = !input.readOnly
        return [
            { text: qsTr("&Undo"), icon: "edit-undo", shortcut: "Ctrl+Z", enabled: editable && input.canUndo, action: () => input.undo() },
            { separator: true },
            { text: qsTr("Cu&t"), icon: "edit-cut", shortcut: "Ctrl+X", enabled: editable && selected, action: () => input.cut() },
            { text: qsTr("&Copy"), icon: "edit-copy", shortcut: "Ctrl+C", enabled: selected, action: () => input.copy() },
            { text: qsTr("&Paste"), icon: "edit-paste", shortcut: "Ctrl+V", enabled: editable && input.canPaste, action: () => input.paste() },
            { text: qsTr("&Delete"), icon: "edit-delete", shortcut: qsTr("Del"), enabled: editable && selected, action: () => input.remove(input.selectionStart, input.selectionEnd) },
            { separator: true },
            { text: qsTr("Select &all"), icon: "edit-select-all", shortcut: "Ctrl+A", enabled: input.text.length > 0, action: () => input.selectAll() }
        ]
    }

    // The window menu (title bar, Alt+Space, Shift+right click on the taskbar
    // button). act(action) runs it. With the window's id, also the virtual
    // desktops: where to move it, and whether it shows on all of them.
    function windowEntries(maximized, minimized, resizable, act, window) {
        const entries = [
            { text: qsTr("&Restore"), icon: "window-restore", enabled: maximized || minimized, action: () => act("restore") },
            { text: qsTr("&Move"), icon: "transform-move", enabled: !maximized && !minimized, action: () => act("move") },
            { text: qsTr("&Size"), icon: "transform-scale", enabled: !maximized && !minimized && resizable, action: () => act("resize") },
            { text: qsTr("Mi&nimize"), icon: "window-minimize", enabled: !minimized, action: () => act("minimize") },
            { text: qsTr("Ma&ximize"), icon: "window-maximize", enabled: !maximized && resizable, action: () => act("maximize") }
        ]
        const desktop = window ? Desktops.workspaceOf(window) : -2 // -1: on all of them; -2: unknown
        if (desktop !== -2) {
            const moveTo = []
            for (let i = 0; i < Desktops.count; ++i) {
                if (i !== desktop) {
                    const index = i
                    moveTo.push({ text: Desktops.names[i].replace(/&/g, "&&"), action: () => Desktops.moveWindow(window, index) })
                }
            }
            if (moveTo.length > 0) {
                moveTo.push({ separator: true })
            }
            moveTo.push({ text: qsTr("&New desktop"), icon: "list-add", action: () => Desktops.moveWindowToNew(window) })
            entries.push(
                { separator: true },
                { text: qsTr("Move to &desktop"), icon: "virtual-desktops", enabled: desktop !== -1, children: moveTo },
                { text: qsTr("Show on &all desktops"), checked: desktop === -1, action: () => Desktops.setWindowSticky(window, desktop !== -1) })
        }
        entries.push(
            { separator: true },
            { text: qsTr("&Close"), icon: "window-close", shortcut: "Alt+F4", action: () => act("close") })
        return entries
    }
}
