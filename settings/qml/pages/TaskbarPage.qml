// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Taskbar: what is on it, its clock and how it behaves
// first, then its style and the style of the quick settings and notification
// center it opens (taskbar-style=, notifications-style= in vela.conf, over the
// theme), each closed under its card until opened. The time format (Date &
// time) and the number of notifications (Notifications) are Vela's and the
// notifications', not the bar's: linked at the end.
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

    // Each icon on the taskbar or in the drawer behind "^" (docs/taskbar.md
    // §8.2): the running apps' icons, read from the shell's registry.
    CardGroup {
        title: qsTr("Notification area")
        Card {
            icon: "go-up"
            title: qsTr("Icons on the taskbar")
            description: TrayIcons.items.length > 0
                ? qsTr("The others wait in the drawer behind ^. New icons start there; the icons of apps that aren't running show here once they start.")
                : qsTr("No app shows an icon in the notification area right now. Icons show here once their apps start.")
        }
        Repeater {
            model: TrayIcons.items
            Card {
                required property var modelData
                objectName: "trayIconCard-" + modelData.key
                icon: modelData.icon
                title: modelData.title
                trailing: Switch {
                    checked: Prefs.taskbarTrayShown.indexOf(modelData.key) >= 0
                    Accessible.name: qsTr("Show %1 on the taskbar").arg(modelData.title)
                    onToggled: Prefs.setTrayIconShown(modelData.key, checked)
                }
            }
        }
    }

    CardGroup {
        title: qsTr("Taskbar behaviors")
        // Disabled, with the reason, while a pill places the apps itself.
        TaskbarAlignmentCard {}
        Card {
            title: qsTr("Show windows from")
            trailing: Choice {
                model: [qsTr("This desktop"), qsTr("All desktops")]
                currentIndex: Prefs.taskbarWindowsFrom === "all" ? 1 : 0
                onChosen: index => Prefs.taskbarWindowsFrom = index === 1 ? "all" : "desktop"
            }
        }
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
            title: qsTr("Show apps on")
            trailing: Choice {
                usable: Prefs.taskbarAllScreens
                model: [qsTr("All taskbars"), qsTr("The taskbar where the window is open")]
                currentIndex: Prefs.taskbarAppsOn === "window" ? 1 : 0
                onChosen: index => Prefs.taskbarAppsOn = index === 1 ? "window" : "all"
            }
        }
        Card {
            title: qsTr("When I click the app in front with several windows")
            trailing: Choice {
                model: [qsTr("Show its windows"), qsTr("Switch to the next one")]
                currentIndex: Prefs.taskbarClickActive === "next" ? 1 : 0
                onChosen: index => Prefs.taskbarClickActive = index === 1 ? "next" : "previews"
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
        title: qsTr("Badges and attention")
        Card {
            title: qsTr("Flash apps that need attention")
            trailing: Switch {
                checked: Prefs.taskbarAttention
                onToggled: Prefs.taskbarAttention = checked
            }
        }
        Card {
            title: qsTr("Show badges on apps")
            trailing: Switch {
                checked: Prefs.taskbarBadges
                onToggled: Prefs.taskbarBadges = checked
            }
        }
        Card {
            title: qsTr("Show progress on apps")
            trailing: Switch {
                checked: Prefs.taskbarProgress
                onToggled: Prefs.taskbarProgress = checked
            }
        }
    }

    CardGroup {
        title: qsTr("Keyboard")
        Card {
            title: qsTr("Super+1…9, 0")
            trailing: Choice {
                model: [qsTr("Taskbar apps"), qsTr("Virtual desktops"), qsTr("Off")]
                currentIndex: Prefs.superNumberKeys === "desktops" ? 1 : Prefs.superNumberKeys === "off" ? 2 : 0
                onChosen: index => Prefs.superNumberKeys = ["apps", "desktops", "off"][index]
            }
        }
        Card {
            title: qsTr("Show numbers while holding Super")
            trailing: Switch {
                enabled: Prefs.superNumberKeys === "apps"
                checked: Prefs.taskbarNumberHints
                onToggled: Prefs.taskbarNumberHints = checked
            }
        }
    }

    CardGroup {
        title: qsTr("Clock")
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
            icon: "chronometer"
            title: qsTr("Show seconds")
            description: qsTr("In the taskbar's clock")
            trailing: Switch {
                objectName: "clockSeconds"
                checked: Prefs.clockSeconds
                onToggled: Prefs.clockSeconds = checked
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

    CardGroup {
        title: qsTr("Related settings")
        LinkCard {
            icon: "preferences-system-time"
            title: qsTr("Time format")
            description: qsTr("24-hour or 12-hour, in Date & time")
            onClicked: root.navigate("datetime")
        }
        LinkCard {
            icon: "preferences-desktop-notification"
            title: qsTr("Notifications")
            description: qsTr("Do not disturb, the number of notifications next to the clock")
            onClicked: root.navigate("notifications")
        }
    }
}
