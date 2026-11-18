// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Context menu: which entries Vela's right-click menus
// show (docs/renderer.md §14.12). One card per menu, opened to a switch per
// entry, then the entries apps add (KDE service menus), then a reset. The
// keys are the ones the menus give their entries (scripts/check-menu-keys.py
// keeps both sides in step). Text fields, the window menu and the tray's
// menus aren't here: the first two are what's left when everything else
// fails, and the tray's are each app's own.
Page {
    id: page

    readonly property var menus: [
        { icon: "user-desktop-symbolic", title: qsTr("Desktop"), about: qsTr("A right click on the desktop's background"), entries: [
            { keys: ["desktop.view"], text: qsTr("View"), icon: "view-list-icons" },
            { keys: ["desktop.sort"], text: qsTr("Sort by"), icon: "view-sort" },
            { keys: ["desktop.refresh"], text: qsTr("Refresh"), icon: "view-refresh" },
            { keys: ["desktop.undo"], text: qsTr("Undo"), icon: "edit-undo" },
            { keys: ["desktop.paste"], text: qsTr("Paste"), icon: "edit-paste" },
            { keys: ["desktop.pasteShortcut"], text: qsTr("Paste shortcut"), icon: "insert-link" },
            { keys: ["desktop.new"], text: qsTr("New"), icon: "list-add" },
            { keys: ["desktop.display"], text: qsTr("Display settings"), icon: "monitor-symbolic" },
            { keys: ["desktop.personalize"], text: qsTr("Personalize"), icon: "draw-brush-symbolic" },
            { keys: ["desktop.terminal"], text: qsTr("Open in Terminal"), icon: "utilities-terminal-symbolic" }
        ] },
        { icon: "document-open", title: qsTr("Files and folders"), about: qsTr("On the desktop and in Files"), entries: [
            { keys: ["file.cut"], text: qsTr("Cut"), icon: "edit-cut" },
            { keys: ["file.copy"], text: qsTr("Copy"), icon: "edit-copy" },
            { keys: ["file.rename"], text: qsTr("Rename"), icon: "edit-rename" },
            { keys: ["file.share"], text: qsTr("Share"), icon: "document-share" },
            { keys: ["file.delete"], text: qsTr("Delete"), icon: "edit-delete" },
            { keys: ["file.open"], text: qsTr("Open"), icon: "document-open" },
            { keys: ["file.newTab"], text: qsTr("Open in new tab (Files)"), icon: "tab-new" },
            { keys: ["file.newWindow"], text: qsTr("Open in new window (Files)"), icon: "window-new" },
            { keys: ["file.openWith"], text: qsTr("Open with"), icon: "document-open" },
            { keys: ["file.location"], text: qsTr("Open file location (Files)"), icon: "folder-open-symbolic" },
            { keys: ["file.pinStart"], text: qsTr("Pin to Start (apps on the desktop)"), icon: "window-pin" },
            { keys: ["file.favorite"], text: qsTr("Add to Favorites"), icon: "starred-symbolic" },
            { keys: ["file.quickAccess"], text: qsTr("Pin to Quick access (Files)"), icon: "window-pin" },
            { keys: ["file.terminal"], text: qsTr("Open in Terminal"), icon: "utilities-terminal-symbolic" },
            { keys: ["file.extract"], text: qsTr("Extract all (Files)"), icon: "archive-extract" },
            { keys: ["file.compress"], text: qsTr("Compress to"), icon: "archive-insert" },
            { keys: ["file.copyPath"], text: qsTr("Copy as path"), icon: "edit-copy-path" },
            { keys: ["file.createShortcut"], text: qsTr("Create shortcut"), icon: "insert-link" },
            { keys: ["file.properties"], text: qsTr("Properties"), icon: "document-properties" }
        ] },
        { icon: "folder-open-symbolic", title: qsTr("Folder background"), about: qsTr("A right click on the empty space of a folder in Files"), entries: [
            { keys: ["folder.view"], text: qsTr("View"), icon: "view-list-icons" },
            { keys: ["folder.sort"], text: qsTr("Sort by"), icon: "view-sort" },
            { keys: ["folder.refresh"], text: qsTr("Refresh"), icon: "view-refresh" },
            { keys: ["folder.undo"], text: qsTr("Undo"), icon: "edit-undo" },
            { keys: ["folder.paste"], text: qsTr("Paste"), icon: "edit-paste" },
            { keys: ["folder.pasteShortcut"], text: qsTr("Paste shortcut"), icon: "insert-link" },
            { keys: ["folder.new"], text: qsTr("New"), icon: "list-add" },
            { keys: ["folder.terminal"], text: qsTr("Open in Terminal"), icon: "utilities-terminal-symbolic" },
            { keys: ["folder.properties"], text: qsTr("Properties"), icon: "document-properties" }
        ] },
        { icon: "preferences-system-windows", title: qsTr("Taskbar apps"), about: qsTr("A right click on an app's button in the taskbar"), entries: [
            { keys: ["taskbar.pinned"], text: qsTr("Pinned files") },
            { keys: ["taskbar.recent"], text: qsTr("Recent files") },
            { keys: ["taskbar.tasks"], text: qsTr("The app's tasks") },
            { keys: ["taskbar.launch"], text: qsTr("The app, to open it again") },
            { keys: ["taskbar.pin"], text: qsTr("Pin to taskbar"), icon: "window-pin" },
            { keys: ["taskbar.close"], text: qsTr("Close window"), icon: "window-close" }
        ] },
        { icon: "view-app-grid-symbolic", title: qsTr("Apps in Start"), about: qsTr("A right click on an app in Start"), entries: [
            { keys: ["start.recent"], text: qsTr("Recent files") },
            { keys: ["start.tasks"], text: qsTr("The app's tasks") },
            { keys: ["start.pin"], text: qsTr("Pin to Start"), icon: "window-pin" },
            { keys: ["start.front"], text: qsTr("Move to front"), icon: "go-top" },
            { keys: ["start.taskbar"], text: qsTr("Pin to taskbar"), icon: "window-pin" },
            { keys: ["start.desktop"], text: qsTr("Add to desktop"), icon: "user-desktop-symbolic" },
            { keys: ["start.location"], text: qsTr("Open file location"), icon: "document-open-folder" },
            { keys: ["start.uninstall"], text: qsTr("Uninstall"), icon: "edit-delete" }
        ] },
        { icon: "start-here", title: qsTr("Start button"), about: qsTr("A right click on Start, or Win+X"), entries: [
            { keys: ["winx.installedApps"], text: qsTr("Installed apps"), icon: "view-app-grid-symbolic" },
            { keys: ["winx.mobility"], text: qsTr("Mobility Center (laptops)"), icon: "computer-laptop-symbolic" },
            { keys: ["winx.power"], text: qsTr("Power options"), icon: "battery-profile-powersave-symbolic" },
            { keys: ["winx.events"], text: qsTr("Event viewer"), icon: "view-list-text-symbolic" },
            { keys: ["winx.system"], text: qsTr("System"), icon: "computer-symbolic" },
            { keys: ["winx.network"], text: qsTr("Network connections"), icon: "preferences-system-network-symbolic" },
            { keys: ["winx.terminal"], text: qsTr("Terminal"), icon: "utilities-terminal-symbolic" },
            { keys: ["winx.terminalAdmin"], text: qsTr("Terminal (Admin)"), icon: "utilities-terminal-symbolic" },
            { keys: ["winx.taskManager"], text: qsTr("Task manager"), icon: "office-chart-area" },
            { keys: ["winx.settings"], text: qsTr("Settings"), icon: "preferences-system-symbolic" },
            { keys: ["winx.files"], text: qsTr("File Explorer"), icon: "system-file-manager-symbolic" },
            { keys: ["winx.search"], text: qsTr("Search"), icon: "search" },
            { keys: ["winx.shutdown"], text: qsTr("Shut down or sign out"), icon: "system-shutdown" },
            { keys: ["winx.desktop"], text: qsTr("Desktop"), icon: "user-desktop-symbolic" }
        ] }
    ]

    // The entries apps add, read when the page opens; an app's submenu is
    // in the name ("Ark › Extract here").
    readonly property var services: ServiceMenus.all().map(s => ({
        keys: [s.key],
        text: s.submenu ? s.submenu + " › " + s.text : s.text,
        icon: s.icon
    }))

    CardGroup {
        title: qsTr("Menus")
        Repeater {
            model: page.menus
            delegate: MenuEntriesCard {
                required property var modelData
                icon: modelData.icon
                title: modelData.title
                about: modelData.about
                entries: modelData.entries
            }
        }
    }

    CardGroup {
        title: qsTr("Added by apps")
        MenuEntriesCard {
            visible: page.services.length > 0
            icon: "plugins"
            title: qsTr("App entries")
            about: qsTr("In the menus of files and folders, before Properties")
            entries: page.services
        }
        Card {
            visible: page.services.length === 0
            icon: "plugins"
            title: qsTr("No app has added entries")
            description: qsTr("Apps such as Ark or Filelight add theirs to the menus of files and folders")
        }
    }

    CardGroup {
        Card {
            icon: "edit-undo"
            title: qsTr("Show every entry again")
            description: qsTr("All the entries you turned off come back in every menu")
            trailing: Button {
                objectName: "showAll"
                text: qsTr("Show all")
                usable: Prefs.hiddenMenuEntries.length > 0
                onClicked: Prefs.showAllMenuEntries()
            }
        }
    }
}
