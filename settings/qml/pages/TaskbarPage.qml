// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Personalization > Taskbar: its style and the style of the quick settings
// and notification center it opens (taskbar-style=, notifications-style= in
// vela.conf), over the theme; what is on it and how it behaves.
Page {
    id: page
    // The taskbar's layout in the theme and style in use, in Vela's mode, by
    // the shell's own rule (Style.taskbarLayout(), themefile.h): a fitted pill
    // places its apps itself, so the alignment below doesn't apply to it.
    readonly property var layout: {
        Prefs.visualTheme; Prefs.surfaceStyles; Prefs.themesRevision
        return Style.taskbarLayout(Prefs.resolveTheme(Prefs.visualTheme).tokens, Prefs.shellTheme === "light",
            Prefs.taskbarAlignment)
    }
    readonly property bool fit: layout.fit
    readonly property bool appsAtLeft: layout.appsAtLeft
    // Who decides it: the style chosen here, or the theme.
    readonly property string layoutOwner: {
        const style = Prefs.surfaceStyles["taskbar"] || ""
        if (style !== "") {
            const entry = Prefs.styleGallery("taskbar").find(s => s.name === style)
            return entry ? entry.text : style
        }
        return Prefs.visualThemes().find(t => t.name === Prefs.visualTheme)?.text ?? Prefs.visualTheme
    }

    DesktopPreview {}

    CardGroup {
        title: qsTr("Style")
        StyleGallery { surface: "taskbar" }
    }

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
        title: qsTr("Taskbar behaviors")
        Card {
            icon: "format-justify-center"
            title: qsTr("Taskbar alignment")
            description: !page.fit ? ""
                : page.appsAtLeft ? qsTr("%1 keeps the apps at the left: the alignment applies with another style").arg(page.layoutOwner)
                : qsTr("%1 keeps the apps centered in its pill: the alignment applies with another style").arg(page.layoutOwner)
            trailing: Choice {
                usable: !page.fit
                model: [qsTr("Center"), qsTr("Left")]
                // What the taskbar does: the style's, while it decides.
                currentIndex: page.fit ? (page.appsAtLeft ? 1 : 0) : Prefs.taskbarAlignment === "left" ? 1 : 0
                onChosen: index => Prefs.taskbarAlignment = index === 1 ? "left" : "center"
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
        title: qsTr("Quick settings and notification center")
        StyleGallery { surface: "notifications" }
    }

    CardGroup {
        Card {
            icon: "edit-entry"
            title: qsTr("Your own styles")
            description: qsTr("A file in ~/.config/vela/themes/taskbar/ or themes/notifications/, <name>.conf, appears here")
        }
    }
}
