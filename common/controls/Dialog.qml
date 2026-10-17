// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The Windows 11 dialog inside a window (ContentDialog): above the content,
// which darkens; title, text, any content (the default property) and up to
// four buttons in a band at the bottom (`buttons`: [{text, accent, value,
// enabled}]). `chosen(value)` with the pressed one; Esc chooses "cancel",
// Enter the accent button. It grows in from 1.05 and fades out (PanelMotion);
// close() without a value closes it without choosing. One choice per
// opening: once it leaves, nothing chooses again.
Item {
    id: dialog
    property string title
    property string text
    property var buttons: [{ text: qsTr("OK"), accent: true, value: "ok" }]
    property int dialogWidth: 460
    default property alias content: extra.data
    readonly property bool closing: motion.closing
    signal chosen(string value)

    anchors.fill: parent
    visible: false
    z: 1000

    function open() {
        visible = true
        motion.open()
        panel.forceActiveFocus()
    }
    function close(value) {
        if (!motion.opened)
            return
        motion.close()
        if (value !== undefined)
            chosen(value)
    }
    // What Enter does: the accent button, if it can be pressed.
    function accept() {
        const accent = buttons.find(b => b.accent)
        if (accent && usable(accent))
            close(accent.value)
    }
    function usable(button) {
        return button.enabled === undefined || button.enabled
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.smoke
        opacity: motion.opacity
        // The content below can't be touched.
        MouseArea { anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.AllButtons; onWheel: {} }
    }

    PanelMotion {
        id: motion
        anchors.centerIn: parent
        width: Math.min(dialog.dialogWidth, dialog.width - 48)
        height: panel.height
        scaleFrom: 1.05
        duration: Theme.normal
        onClosed: dialog.visible = false

        Shadow {
            target: panel
            depth: Theme.shadow.dialog
        }

        Rectangle {
            id: panel
            width: parent.width
            height: body.height + footer.height
            radius: Theme.radiusOverlay
            color: Theme.dialog
            border.width: 1
            border.color: Theme.dialogStroke
            Keys.onEscapePressed: dialog.close("cancel")
            Keys.onReturnPressed: dialog.accept()
            Keys.onEnterPressed: dialog.accept()

            Column {
                id: body
                x: 24
                width: parent.width - 48
                topPadding: 24
                bottomPadding: 24
                spacing: 12
                Text {
                    width: parent.width
                    text: dialog.title
                    visible: text !== ""
                    color: Theme.text
                    font.pixelSize: Theme.fontSubtitle
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    text: dialog.text
                    visible: text !== ""
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.Wrap
                }
                Item {
                    id: extra
                    width: parent.width
                    height: childrenRect.height
                    visible: children.length > 0
                }
            }
            Rectangle {
                id: footer
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 1 }
                height: 80
                color: Theme.dialogFooter
                bottomLeftRadius: Theme.radiusOverlay - 1
                bottomRightRadius: Theme.radiusOverlay - 1
                Row {
                    anchors { fill: parent; margins: 24 }
                    spacing: 8
                    Repeater {
                        model: dialog.buttons
                        delegate: Button {
                            required property var modelData
                            objectName: "button:" + modelData.value
                            width: (parent.width - 8 * (dialog.buttons.length - 1)) / dialog.buttons.length
                            text: modelData.text
                            accent: modelData.accent === true
                            usable: dialog.usable(modelData)
                            onClicked: dialog.close(modelData.value)
                        }
                    }
                }
            }
        }
    }
}
