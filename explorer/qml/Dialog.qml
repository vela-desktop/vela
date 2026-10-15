// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A Windows 11 dialog inside Explorer: above the content, which darkens;
// title, text and up to four buttons at the bottom (`buttons`: [{text, accent,
// value}]); `chosen(value)` with the pressed one (Esc: "cancel").
Item {
    id: dialog
    property string title
    property string text
    property var buttons: [{ text: "OK", accent: true, value: "ok" }]
    property int dialogWidth: 460
    default property alias content: extra.data
    signal chosen(string value)

    anchors.fill: parent
    visible: false
    z: 200

    function open() {
        visible = true
        panel.opacity = 0
        panel.scale = 1 + 0.05 * Theme.motion
        appear.restart()
        panel.forceActiveFocus()
    }
    function close(value) {
        visible = false
        chosen(value)
    }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: panel; property: "opacity"; to: 1; duration: Theme.fast }
        NumberAnimation { target: panel; property: "scale"; to: 1; duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.3)
        MouseArea { anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.AllButtons; onWheel: {} }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(dialog.dialogWidth, parent.width - 48)
        height: body.height + footer.height
        radius: Theme.radiusOverlay
        color: Theme.dialog
        border.width: 1
        border.color: Qt.rgba(0, 0, 0, 0.45)
        focus: dialog.visible
        Keys.onEscapePressed: dialog.close("cancel")
        Keys.onReturnPressed: {
            const accent = dialog.buttons.find(b => b.accent)
            if (accent) dialog.close(accent.value)
        }

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
                font.pixelSize: 20
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
                    delegate: Rectangle {
                        id: button
                        required property var modelData
                        width: (parent.width - 8 * (dialog.buttons.length - 1)) / dialog.buttons.length
                        height: 32
                        radius: Theme.radius
                        color: modelData.accent ? (buttonMouse.containsMouse ? (Theme.light ? Qt.darker(Theme.accent, 1.05) : Qt.lighter(Theme.accent, 1.25)) : Theme.accentFill)
                                                : (buttonMouse.containsMouse ? Theme.controlHover : Theme.control)
                        border.width: 1
                        border.color: Theme.controlStroke
                        Text {
                            anchors.centerIn: parent
                            text: button.modelData.text
                            color: button.modelData.accent ? Theme.accentText : Theme.text
                            font.pixelSize: Theme.fontBody
                        }
                        MouseArea {
                            id: buttonMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: dialog.close(button.modelData.value)
                        }
                    }
                }
            }
        }
    }
}
