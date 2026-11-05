// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// What a click just changed, with Undo for a few seconds: themes and styles
// apply at once (Personalization), so going back is one click too. offer()
// replaces what was offered before; Undo, the close button or the time
// running out take it away. It stays across pages: what it undoes doesn't
// depend on the page.
Item {
    id: bar
    property string text
    property var undo: null
    readonly property bool shown: undo !== null
    // Long enough to read it and find the button, not so long it stays in
    // the way: what Windows gives its notifications.
    property int timeout: 8000

    function offer(text, undo) {
        bar.text = text
        bar.undo = undo
        timer.restart()
    }
    function dismiss() {
        bar.undo = null
        timer.stop()
    }

    implicitWidth: Math.min(560, metrics.advanceWidth + undoButton.implicitWidth + closeButton.implicitWidth + 2 * row.spacing + 28)
    implicitHeight: 56
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    // Doesn't go away under the pointer: whoever is reaching for Undo gets it.
    HoverHandler { id: hover }

    Timer {
        id: timer
        interval: bar.timeout
        running: false
        onTriggered: hover.hovered ? restart() : bar.dismiss()
    }

    TextMetrics { id: metrics; text: bar.text; font.pixelSize: Theme.fontBody }
    Shadow { target: panel; radius: panel.radius }
    Fill {
        id: panel
        anchors.fill: parent
        radius: Theme.radiusOverlay
        // Opaque: inside the window nothing blurs what is under it, and the
        // page's cards would show through.
        color: Theme.light ? "#f9f9f9" : "#2c2c2c"
        border.width: 1
        border.color: Theme.cardStroke
    }
    Row {
        id: row
        anchors { left: parent.left; leftMargin: 16; right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
        spacing: 12
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - undoButton.width - closeButton.width - 2 * parent.spacing
            text: bar.text
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
        Button {
            id: undoButton
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Undo")
            onClicked: {
                const undo = bar.undo
                bar.dismiss()
                if (undo) {
                    undo()
                }
            }
        }
        Button {
            id: closeButton
            anchors.verticalCenter: parent.verticalCenter
            subtle: true
            icon.name: "window-close"
            Accessible.name: qsTr("Close")
            onClicked: bar.dismiss()
        }
    }
}
