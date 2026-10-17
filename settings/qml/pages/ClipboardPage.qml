// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// System > Clipboard, like Windows 11: clipboard history (Win+V) and "Clear
// clipboard data" (except pinned items).
Page {
    CardGroup {
        Card {
            icon: "edit-paste"
            title: qsTr("Clipboard history")
            description: qsTr("Save multiple items to the clipboard to use later. Press Win+V to see and paste them")
            trailing: Switch {
                checked: Prefs.clipboardHistory
                onToggled: Prefs.clipboardHistory = checked
            }
        }
        Card {
            icon: "edit-clear-history"
            title: qsTr("Clear clipboard data")
            description: qsTr("Everything except pinned items")
            trailing: Button {
                text: qsTr("Clear")
                onClicked: Prefs.clearClipboard()
            }
        }
    }
    CardGroup {
        title: qsTr("Related settings")
        Card {
            icon: "accessories-screenshot"
            title: qsTr("Snipping Tool")
            description: qsTr("Win+Shift+S or Print Screen: a rectangle, a window or the full screen, copied to the clipboard and saved in Pictures > Screenshot")
        }
    }
}
