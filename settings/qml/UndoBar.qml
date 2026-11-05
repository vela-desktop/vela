// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// What a click just changed, with Undo for a few seconds: themes and styles
// apply at once (Personalization), so going back is one click too. offer()
// replaces what was offered before; Undo, the close button or the time
// running out take it away. It stays across pages: what it undoes doesn't
// depend on the page. Pages keep room at their end for it (Page.qml), and
// screen readers read it out when it comes.
Item {
    id: bar
    property string text
    property var undo: null
    readonly property bool shown: undo !== null
    // Long enough to read it and find the button, not so long it stays in
    // the way: what Windows gives its notifications.
    property int timeout: 8000
    // Whoever is reaching for Undo gets it: the time doesn't run out under
    // the pointer, nor while the keyboard is on its buttons.
    readonly property bool held: hover.hovered || undoButton.activeFocus || closeButton.activeFocus

    Accessible.role: Accessible.AlertMessage
    Accessible.name: bar.text

    function offer(text, undo) {
        bar.text = text
        bar.undo = undo
        timer.restart()
        // Said, not only shown (Qt 6.8 and later).
        if (typeof bar.Accessible.announce === "function") {
            bar.Accessible.announce(text)
        }
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
    HoverHandler { id: hover }

    Timer {
        id: timer
        interval: bar.timeout
        running: false
        onTriggered: bar.held ? restart() : bar.dismiss()
    }

    TextMetrics { id: metrics; text: bar.text; font.pixelSize: Theme.fontBody }
    Shadow { target: panel; radius: panel.radius }
    Fill {
        id: panel
        objectName: "undoPanel"
        anchors.fill: parent
        radius: Theme.radiusOverlay
        // A flyout's, opaque and themed: inside the window nothing blurs what
        // is under it, and the page's cards would show through.
        color: Theme.flyout
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
