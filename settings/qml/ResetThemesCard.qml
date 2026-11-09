// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Themes' Reset: Vela Official and no styles, the mode and the accent left
// as they are. Undo, for a few seconds, puts the theme and every style back
// at once (UndoBar), in one write of vela.conf.
Card {
    id: card
    // Vela Official's name, as its file gives it.
    readonly property string velaName: { Prefs.themesRevision; return Prefs.displayName("", "vela") || "Vela Official" }

    icon: "edit-undo"
    title: qsTr("Reset")
    description: qsTr("%1 and no styles. Light or dark mode and the accent stay").arg(velaName)
    trailing: Button {
        objectName: "reset"
        text: qsTr("Reset")
        usable: Prefs.visualTheme !== "vela" || Object.values(Prefs.surfaceStyles).some(s => s !== "")
        onClicked: {
            const theme = Prefs.visualTheme
            const styles = Object.assign({}, Prefs.surfaceStyles)
            Prefs.resetThemes()
            if (Prefs.saveError !== "") {
                return
            }
            root.offerUndo(qsTr("%1 is in use, with no styles").arg(card.velaName),
                () => Prefs.setThemeAndStyles(theme, styles))
        }
    }
}
