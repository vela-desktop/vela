// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
import Vela.Controls

// Themes' summary of the look in use, the layers that make it in one card:
// the theme, each surface's style over it, and where the accent color comes
// from. Each line opens the page where it is chosen, the style's own card
// opened (Main.qml's navigate() with what to reveal); nothing changes here.
Card {
    id: card
    readonly property var tokens: { Prefs.visualTheme; Prefs.surfaceStyles; Prefs.themesRevision; return Prefs.resolveTheme(Prefs.visualTheme).tokens }
    readonly property string themeName: { Prefs.themesRevision; return Prefs.displayName("", Prefs.visualTheme) || Prefs.visualTheme }
    readonly property bool accentDark: "accent" in tokens || "dark.accent" in tokens
    readonly property bool accentLight: "accent" in tokens || "light.accent" in tokens

    function styleName(surface) {
        Prefs.themesRevision
        const chosen = Prefs.surfaceStyles[surface] || ""
        if (chosen === "") {
            return qsTr("From the theme")
        }
        return Prefs.displayName(surface, chosen) || qsTr("\"%1\" isn't installed").arg(chosen)
    }

    icon: "preferences-desktop-theme"
    title: qsTr("In use: %1").arg(themeName)
    description: qsTr("The theme, and the styles and accent color over it")
    contentItem: Column {
        x: 36
        width: parent.width - 36
        bottomPadding: 4
        Line {
            objectName: "inUseTaskbar"
            label: qsTr("Taskbar")
            value: card.styleName("taskbar")
            page: "taskbar"
            reveal: "taskbar"
        }
        Line {
            objectName: "inUseStart"
            label: qsTr("Start menu")
            value: card.styleName("startmenu")
            page: "start"
        }
        Line {
            objectName: "inUseNotifications"
            label: qsTr("Quick settings and notification center")
            value: card.styleName("notifications")
            page: "taskbar"
            reveal: "notifications"
        }
        Line {
            objectName: "inUseAccent"
            label: qsTr("Accent color")
            value: card.accentDark && card.accentLight ? qsTr("The theme's")
                : card.accentDark ? qsTr("The theme's in dark mode, yours in light mode")
                : card.accentLight ? qsTr("The theme's in light mode, yours in dark mode")
                : qsTr("Yours")
            page: "colors"
        }
    }

    // A line: what, which, and the way to the page that changes it.
    component Line: T.AbstractButton {
        id: line
        property string label
        property string value
        property string page
        property string reveal
        width: parent ? parent.width : 0
        implicitHeight: 36
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.Button
        Accessible.name: label + ": " + value
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        onClicked: root.navigate(page, false, reveal)

        background: Fill {
            radius: Theme.radius
            color: line.down ? Theme.subtlePressed : line.hovered ? Theme.subtleHover : Theme.subtleRest
            FocusRing {
                visible: line.visualFocus
            }
        }
        contentItem: Item {
            Text {
                id: labelText
                anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                width: Math.min(implicitWidth, parent.width * 0.5)
                text: line.label
                textFormat: Text.PlainText
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
            Text {
                anchors { left: labelText.right; leftMargin: 16; right: chevron.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                text: line.value
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontBody
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
            }
            Chevron {
                id: chevron
                anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                color: Theme.textSecondary
            }
        }
    }
}
