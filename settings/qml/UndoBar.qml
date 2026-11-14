// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes
import Vela.Controls

// What a click just changed, with Undo for a few seconds: themes and styles
// apply at once (Personalization), so going back is one click too. offer()
// replaces what was offered before; Undo, the close button or the time
// running out take it away. It stays across pages: what it undoes doesn't
// depend on the page. Pages keep room at their end for it (Page.qml), and
// screen readers read it out when it comes.
//
// Expressive: M3's snackbar: inverse surface with extra small corners and
// no outline, the text in inverse on surface, Undo a text button in inverse
// primary, the close mark drawn in inverse on surface (the icon theme has
// no glyph for a light inverse surface: Vela's choice).
Item {
    id: bar
    readonly property bool expressive: Theme.expressive
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

    // No wider than this (Main.qml: the page's width): a long text elides.
    property real maximumWidth: 560

    implicitWidth: Math.min(maximumWidth, metrics.advanceWidth + undoButton.implicitWidth + closeButton.implicitWidth + 2 * row.spacing + 28)
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
        radius: bar.expressive ? Theme.shapeExtraSmall : Theme.radiusOverlay
        // A flyout's, opaque and themed: inside the window nothing blurs what
        // is under it, and the page's cards would show through.
        color: bar.expressive ? Theme.inverseSurface : Theme.flyout
        border.width: bar.expressive ? 0 : 1
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
            color: bar.expressive ? Theme.inverseSurfaceText : Theme.text
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
        Button {
            id: undoButton
            objectName: "undoButton"
            anchors.verticalCenter: parent.verticalCenter
            subtle: bar.expressive
            onInverse: bar.expressive
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
            objectName: "closeButton"
            subtle: true
            onInverse: bar.expressive
            icon.name: bar.expressive ? "" : "window-close"
            Accessible.name: qsTr("Close")
            onClicked: bar.dismiss()
            Shape {
                objectName: "closeMark"
                visible: bar.expressive
                anchors.centerIn: parent
                width: 10
                height: 10
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: Theme.inverseSurfaceText
                    strokeWidth: 1.5
                    capStyle: ShapePath.RoundCap
                    startX: 0; startY: 0
                    PathLine { x: 10; y: 10 }
                    PathMove { x: 10; y: 0 }
                    PathLine { x: 0; y: 10 }
                }
            }
        }
    }
}
