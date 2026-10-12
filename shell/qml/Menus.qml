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
// (x is the center), keyboard (opened from the keyboard), minWidth, rebuild
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
    // The window previews of a taskbar button (TaskbarPreview.qml): {key,
    // appIds, center (x on the output)}, or null. They stay while the mouse is
    // on the button or the previews.
    property var preview: null
    // A tooltip above the taskbar (TaskbarTooltip.qml): {text, center (x on
    // the output), screen}, or null.
    property var tooltip: null
    // The output of the taskbar a panel was opened from (Start, quick
    // settings, notification center, Task View): the panel goes there. Empty:
    // the main output (opened from the keyboard).
    property string panelScreen: ""
    function targetScreen() {
        const name = panelScreen !== "" ? panelScreen : Shell.primaryScreen
        panelScreen = ""
        return Qt.application.screens.find(s => s.name === name) || null
    }
    // Puts `window` (hidden) on the panel's output.
    function placeOnTargetScreen(window) {
        const screen = targetScreen()
        if (screen) {
            Shell.placeOnScreen(window, screen.name)
        }
    }
    property bool previewHovered: false
    property bool previewButtonHovered: false

    signal openRequested(var entries, real x, real y, var options)
    signal closeRequested()
    // A Yes/No question (such as "Empty Recycle Bin"): ConfirmDialog.qml.
    signal confirmRequested(string title, string text, string yesText, var action)

    function confirm(title, text, yesText, action) {
        confirmRequested(title, text, yesText, action)
    }

    // The Properties window (PropertiesDialog.qml) for these files.
    signal propertiesRequested(var paths)
    function showProperties(paths) {
        propertiesRequested(paths)
    }

    // Share, "Choose another app", New > Shortcut (FileDialogs.qml).
    signal shareRequested(var paths)
    signal openWithRequested(string path)
    signal newShortcutRequested(string folder)
    function share(paths) { shareRequested(paths) }
    function chooseApp(path) { openWithRequested(path) }
    function newShortcut(folder) { newShortcutRequested(folder) }

    function open(entries, x, y, options) {
        isOpen = true
        openRequested(entries, x, y, options || {})
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
    // button). act(action) runs it.
    function windowEntries(maximized, minimized, resizable, act) {
        return [
            { text: qsTr("&Restore"), icon: "window-restore", enabled: maximized || minimized, action: () => act("restore") },
            { text: qsTr("&Move"), enabled: !maximized && !minimized, action: () => act("move") },
            { text: qsTr("&Size"), enabled: !maximized && !minimized && resizable, action: () => act("resize") },
            { text: qsTr("Mi&nimize"), icon: "window-minimize", enabled: !minimized, action: () => act("minimize") },
            { text: qsTr("Ma&ximize"), icon: "window-maximize", enabled: !maximized && resizable, action: () => act("maximize") },
            { separator: true },
            { text: qsTr("&Close"), icon: "window-close", shortcut: "Alt+F4", action: () => act("close") }
        ]
    }
}
