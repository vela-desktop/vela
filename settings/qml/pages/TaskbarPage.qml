// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Taskbar: what is on it, its clock and how it behaves
// first, then its style and the style of the quick settings and notification
// center it opens (taskbar-style=, notifications-style= in vela.conf, over the
// theme), each closed under its card until opened.
Page {
    DesktopPreview {}

    CardGroup {
        title: qsTr("Taskbar items")
        Card {
            icon: "view-grid"
            title: qsTr("Task view")
            description: qsTr("Open windows and virtual desktops (also with Win+Tab)")
            trailing: Switch {
                checked: Prefs.taskView
                onToggled: Prefs.taskView = checked
            }
        }
    }

    CardGroup {
        title: qsTr("Clock and notifications")
        Card {
            id: hours
            icon: "preferences-system-time"
            title: qsTr("Time format")
            description: qsTr("In the taskbar, the notification center, the lock screen and Date & time")
            // Each choice shows a time as it would read: 13:45 or 1:45 PM.
            readonly property date sample: new Date(2026, 0, 14, 13, 45)
            trailing: Choice {
                objectName: "clockHours"
                model: [qsTr("24-hour (%1)").arg(Qt.formatTime(hours.sample, "HH:mm")),
                    qsTr("12-hour (%1)").arg(Qt.formatTime(hours.sample, "h:mm AP"))]
                currentIndex: Prefs.clock24 ? 0 : 1
                onChosen: index => Prefs.clock24 = index === 0
            }
        }
        Card {
            icon: "chronometer"
            title: qsTr("Show seconds")
            description: qsTr("In the taskbar's clock")
            trailing: Switch {
                objectName: "clockSeconds"
                checked: Prefs.clockSeconds
                onToggled: Prefs.clockSeconds = checked
            }
        }
        Card {
            icon: "view-calendar"
            title: qsTr("Show the date")
            description: qsTr("Under the time in the taskbar's clock")
            trailing: Switch {
                objectName: "clockDate"
                checked: Prefs.clockDate
                onToggled: Prefs.clockDate = checked
            }
        }
        Card {
            icon: "preferences-desktop-notification"
            title: qsTr("Show the number of notifications")
            description: qsTr("Next to the clock. With Do not disturb on, its bell shows anyway")
            trailing: Switch {
                objectName: "notificationCount"
                checked: Prefs.notificationCount
                onToggled: Prefs.notificationCount = checked
            }
        }
    }

    CardGroup {
        title: qsTr("Taskbar behaviors")
        // Disabled, with the reason, while a pill places the apps itself.
        TaskbarAlignmentCard {}
        Card {
            icon: "video-display"
            title: qsTr("Show the taskbar on all displays")
            description: qsTr("With multiple monitors: Start, apps and the clock on each one; notification area on the main one")
            trailing: Switch {
                checked: Prefs.taskbarAllScreens
                onToggled: Prefs.taskbarAllScreens = checked
            }
        }
        Card {
            icon: "user-desktop"
            title: qsTr("Show desktop from taskbar corner")
            description: qsTr("Click the far-right edge of any taskbar to show or restore your windows (also with Super+D)")
            trailing: Switch {
                checked: Prefs.showDesktopCorner
                onToggled: Prefs.showDesktopCorner = checked
            }
        }
        Card {
            icon: "process-stop"
            title: qsTr("End task")
            description: qsTr("In the taskbar buttons' menu, to close an app that isn't responding right away")
            trailing: Switch {
                checked: Prefs.endTask
                onToggled: Prefs.endTask = checked
            }
        }
    }

    CardGroup {
        title: qsTr("Styles")
        StyleChooser {
            surface: "taskbar"
            icon: "preferences-system-windows"
            title: qsTr("Taskbar")
        }
        StyleChooser {
            surface: "notifications"
            icon: "preferences-desktop-notification"
            title: qsTr("Quick settings and notification center")
        }
    }
}
