// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Apps > Default apps, like Windows 11: a specific file or link type, the
// common uses, and the apps one by one (DefaultAppPage.qml).
Page {
    id: page

    // Recomputed when a choice changes (DefaultApps.changed).
    readonly property var result: {
        DefaultApps.categories
        return DefaultApps.lookup(typeField.text)
    }
    readonly property var appList: {
        DefaultApps.categories
        return DefaultApps.apps(appFilter.text)
    }

    Text {
        width: parent.width
        text: qsTr("Choose the apps that open web pages, mail, music, videos, photos, PDFs and other files. Your choices apply to every app.")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontBody
        wrapMode: Text.Wrap
        bottomPadding: 8
    }

    // --- a specific type: ".mkv", "mp3", "mailto" ---
    CardGroup {
        title: qsTr("Set a default for a file type or link type")
        Card {
            icon: "search"
            title: qsTr("File type or link type")
            description: qsTr("For example .pdf, mp3, .mkv or mailto")
            trailing: TextBox {
                id: typeField
                width: 220
                placeholderText: qsTr("Type a file type")
            }
        }
        Card {
            visible: typeField.text.trim() !== "" && page.result.found
            icon: page.result.icon || "unknown"
            title: page.result.label || ""
            description: (page.result.patterns ? page.result.patterns + " · " : "") + (page.result.mime || "")
            trailing: Choice {
                usable: (page.result.candidates || []).length > 0
                model: (page.result.candidates || []).map(a => ({ text: a.name }))
                currentIndex: (page.result.candidates || []).findIndex(a => a.id === (page.result.current || {}).id)
                displayText: currentIndex >= 0 ? page.result.candidates[currentIndex].name
                    : usable ? qsTr("Choose an app") : qsTr("No app opens it")
                onChosen: index => DefaultApps.setDefaultForType(page.result.mime, page.result.candidates[index].id)
            }
        }
        Card {
            visible: typeField.text.trim() !== "" && !page.result.found
            icon: "dialog-information"
            title: qsTr("No type found for «") + typeField.text.trim() + "»"
            description: qsTr("Try the file extension (.pdf) or the name of a link type (mailto, https).")
        }
    }

    // --- common uses ---
    CardGroup {
        title: qsTr("Common uses")
        Repeater {
            model: DefaultApps.categories
            delegate: Card {
                id: category
                required property var modelData
                icon: modelData.current.icon || modelData.icon
                title: modelData.label
                description: modelData.current.name || (modelData.candidates.length > 0 ? qsTr("No app chosen") : qsTr("No installed app opens it"))
                trailing: Choice {
                    usable: category.modelData.candidates.length > 0
                    model: category.modelData.candidates.map(a => ({ text: a.name }))
                    currentIndex: category.modelData.candidates.findIndex(a => a.id === category.modelData.current.id)
                    displayText: currentIndex >= 0 ? category.modelData.candidates[currentIndex].name : qsTr("Choose an app")
                    onChosen: index => DefaultApps.setDefault(category.modelData.key, category.modelData.candidates[index].id)
                }
            }
        }
        // The terminal for Win+Enter and updates (docs/keyboard.md §11):
        // Automatic, or one of the installed terminals.
        Card {
            id: terminalCard
            readonly property var terminal: DefaultApps.terminal
            readonly property string automaticName: (terminal.automatic || {}).name || ""
            icon: terminal.current.icon || (terminal.automatic || {}).icon || "utilities-terminal"
            title: qsTr("Terminal")
            description: terminal.current.name ? qsTr("Opens with Win+Enter and for updates")
                : automaticName ? qsTr("Automatic: %1").arg(automaticName) : qsTr("No terminal installed")
            trailing: Choice {
                objectName: "terminalChoice"
                model: [{ text: qsTr("Automatic") }].concat(terminalCard.terminal.candidates.map(a => ({ text: a.name })))
                currentIndex: terminalCard.terminal.current.id
                    ? 1 + terminalCard.terminal.candidates.findIndex(a => a.id === terminalCard.terminal.current.id) : 0
                displayText: currentIndex > 0 ? terminalCard.terminal.candidates[currentIndex - 1].name : qsTr("Automatic")
                onChosen: index => DefaultApps.setTerminal(index === 0 ? "" : terminalCard.terminal.candidates[index - 1].id)
            }
        }
    }

    // --- per app ---
    CardGroup {
        title: qsTr("Apps")
        Item {
            width: parent.width
            height: 44
            TextBox {
                id: appFilter
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(360, parent.width)
                placeholderText: qsTr("Search apps")
            }
        }
        Repeater {
            model: page.appList
            delegate: Card {
                required property var modelData
                icon: modelData.icon
                title: modelData.name
                description: modelData.count === 1 ? qsTr("Opens 1 file or link type")
                    : qsTr("Opens ") + modelData.count + qsTr(" file and link types")
                clickable: true
                chevron: true
                onClicked: {
                    DefaultApps.selectedApp = modelData.id
                    root.navigate("default-app")
                }
            }
        }
    }
}
