// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Taskbar's alignment: where the apps go, Start's button first. A fitted pill
// places them itself (Style.taskbarLayout(), the shell's own rule), so while
// the theme or a taskbar style draws one the drop-down is disabled, shows
// what the pill does, and says which of the two decides: another style, or
// another theme, gives the choice back.
Card {
    id: card
    readonly property var resolved: { Prefs.visualTheme; Prefs.surfaceStyles; Prefs.themesRevision; return Prefs.resolveTheme(Prefs.visualTheme) }
    // In Vela's mode, the taskbar's.
    readonly property var layout: Style.taskbarLayout(resolved.tokens, Prefs.shellTheme === "light", Prefs.taskbarAlignment)
    // The taskbar style chosen sets the layout over the theme's.
    readonly property bool byStyle: resolved.layoutFromStyle === true
    readonly property string owner: {
        if (byStyle) {
            const style = Prefs.surfaceStyles["taskbar"] || ""
            const entry = Prefs.styleGallery("taskbar").find(s => s.name === style)
            return entry ? entry.text : style
        }
        return Prefs.visualThemes().find(t => t.name === Prefs.visualTheme)?.text ?? Prefs.visualTheme
    }

    icon: "format-justify-center"
    title: qsTr("Taskbar alignment")
    description: !layout.fit ? ""
        : layout.appsAtLeft && byStyle ? qsTr("%1 keeps the apps at the left: the alignment applies with another style").arg(owner)
        : layout.appsAtLeft ? qsTr("%1 keeps the apps at the left: the alignment applies with another theme").arg(owner)
        : byStyle ? qsTr("%1 keeps the apps centered in its pill: the alignment applies with another style").arg(owner)
        : qsTr("%1 keeps the apps centered in its pill: the alignment applies with another theme").arg(owner)
    trailing: Choice {
        objectName: "alignment"
        usable: !card.layout.fit
        model: [qsTr("Center"), qsTr("Left")]
        // What the taskbar does: the pill's, while it decides.
        currentIndex: card.layout.appsAtLeft ? 1 : 0
        onChosen: index => Prefs.taskbarAlignment = index === 1 ? "left" : "center"
    }
}
