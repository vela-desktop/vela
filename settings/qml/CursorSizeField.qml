// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic as C
import Vela.Controls

// The cursor size: a field to type it and − and + beside it, always on one
// of the sizes the cursor theme is drawn at (Preferences.nearestCursorSize()
// and steppedCursorSize()): from 24, + goes to the theme's next size, and a
// typed size to the nearest. The question mark opens why
// (docs/system-appearance.md §2).
Row {
    id: field
    // Settings' Preferences (the page's).
    required property var prefs
    readonly property int shown: prefs.nearestCursorSize(prefs.cursorSize)
    readonly property var sizes: prefs.cursorSizes
    spacing: 4

    function choose(size) {
        field.prefs.cursorSize = field.prefs.nearestCursorSize(size)
        // Back to what was saved: a typed size shows where it landed.
        box.text = String(field.shown)
    }

    Button {
        objectName: "smaller"
        anchors.verticalCenter: parent.verticalCenter
        width: 40
        text: "\u2212" // −: theme icons for it vary (Breeze's is a cross)
        Accessible.name: qsTr("Smaller cursor")
        usable: field.sizes.length === 0 ? field.shown > 8 : field.shown > field.sizes[0]
        onClicked: field.choose(field.prefs.steppedCursorSize(field.shown, -1))
    }
    TextBox {
        id: box
        objectName: "size"
        anchors.verticalCenter: parent.verticalCenter
        width: 64
        horizontalAlignment: TextInput.AlignHCenter
        text: String(field.shown)
        inputMethodHints: Qt.ImhDigitsOnly
        validator: IntValidator { bottom: 1; top: 999 }
        Accessible.name: qsTr("Cursor size in pixels")
        onEditingFinished: field.choose(parseInt(text, 10) || field.shown)
        // A new theme or size from elsewhere shows at once, unless typing.
        Connections {
            target: field
            function onShownChanged() {
                if (!box.activeFocus)
                    box.text = String(field.shown)
            }
        }
    }
    Button {
        objectName: "larger"
        anchors.verticalCenter: parent.verticalCenter
        width: 40
        text: "+"
        Accessible.name: qsTr("Larger cursor")
        usable: field.sizes.length === 0 ? field.shown < 256 : field.shown < field.sizes[field.sizes.length - 1]
        onClicked: field.choose(field.prefs.steppedCursorSize(field.shown, 1))
    }
    // The question mark: a click opens why the size behaves so.
    Button {
        id: help
        objectName: "why"
        anchors.verticalCenter: parent.verticalCenter
        width: 32
        subtle: true
        text: "?"
        checked: why.visible
        Accessible.name: qsTr("Why these sizes")
        Accessible.description: help.explanation
        readonly property string explanation: field.sizes.length === 0
            ? qsTr("This cursor theme's sizes can't be read, so any size from 8 to 256 is accepted. The pointer still shows the theme's drawing nearest to it.")
            : qsTr("A cursor theme is drawn at a few sizes only, %1 px for this one. Other sizes would be the nearest drawing, unchanged, so the size always snaps to one of them: − and + go to the previous or next one, and a typed size goes to the nearest.\n\nThe size is at 100% scale. On a display at another scale the theme is loaded at the size times the scale, and the drawing nearest to that is shown: at 125%, 24 asks for 30, and a theme without 30 shows its nearest, which can be smaller or larger than you expect.")
                .arg(field.sizes.join(", "))
        onClicked: why.visible ? why.close() : why.open()

        // A flyout under the mark, as Choice's list: closed by a click
        // elsewhere or Esc.
        C.Popup {
            id: why
            objectName: "whyPopup"
            // Below the mark, or above it when the window has no room
            // below: never over the field it explains.
            y: {
                const overlay = C.Overlay.overlay
                const bottom = overlay ? help.mapToItem(overlay, 0, help.height).y : 0
                return overlay && bottom + 4 + height + margins > overlay.height ? -height - 4 : help.height + 4
            }
            x: help.width - width
            width: 380
            padding: 16
            margins: 8
            focus: true
            contentItem: Column {
                spacing: 8
                Text {
                    width: parent.width
                    text: qsTr("Why these sizes")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                Text {
                    objectName: "whyText"
                    width: parent.width
                    text: help.explanation
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    lineHeight: 1.15
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
            }
            background: Rectangle {
                color: Theme.flyout
                radius: Theme.radiusOverlay
                border.width: Theme.controlsVariant === "expressive" ? 0 : 1
                border.color: Qt.rgba(0, 0, 0, 0.4)
            }
            enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.fast } }
        }
    }
}
