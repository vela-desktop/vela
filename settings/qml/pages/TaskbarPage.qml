// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Taskbar.
Page {
    PersonalizationPage.DesktopPreview {}

    CardGroup {
        title: qsTr("Taskbar items")
        Card {
            icon: "view-grid"
            title: qsTr("Task view")
            description: qsTr("Open windows and virtual desktops (also with Win+Tab)")
            trailing: Toggle {
                checked: Prefs.taskView
                onToggled: on => Prefs.taskView = on
            }
        }
    }

    CardGroup {
        title: qsTr("Taskbar behaviors")
        Card {
            icon: "format-justify-center"
            title: qsTr("Taskbar alignment")
            trailing: Choice {
                model: [qsTr("Center"), qsTr("Left")]
                currentIndex: Prefs.taskbarAlignment === "left" ? 1 : 0
                onChosen: index => Prefs.taskbarAlignment = index === 1 ? "left" : "center"
            }
        }
        Card {
            icon: "video-display"
            title: qsTr("Show the taskbar on all displays")
            description: qsTr("With multiple monitors: Start, apps and the clock on each one; notification area on the main one")
            trailing: Toggle {
                checked: Prefs.taskbarAllScreens
                onToggled: on => Prefs.taskbarAllScreens = on
            }
        }
        Card {
            icon: "user-desktop"
            title: qsTr("Show desktop from taskbar corner")
            description: qsTr("Click the far-right edge of any taskbar to show or restore your windows (also with Super+D)")
            trailing: Toggle {
                checked: Prefs.showDesktopCorner
                onToggled: on => Prefs.showDesktopCorner = on
            }
        }
        Card {
            icon: "process-stop"
            title: qsTr("End task")
            description: qsTr("In the taskbar buttons' menu, to close an app that isn't responding right away")
            trailing: Toggle {
                checked: Prefs.endTask
                onToggled: on => Prefs.endTask = on
            }
        }
    }
}
