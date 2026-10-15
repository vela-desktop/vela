// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The Windows 11 dialog (ContentDialog): above the page, which darkens; title,
// content, and the buttons in a band at the bottom.
Item {
    id: dialog
    property string title
    property string primaryText: "OK"
    property string secondaryText: qsTr("Cancel")
    property bool primaryEnabled: true
    property int dialogWidth: 448
    default property alias content: body.data
    signal accepted()
    signal rejected()

    anchors.fill: parent
    visible: false
    z: 1000

    function open() {
        visible = true
        panel.scale = 1 + 0.05 * Theme.motion
        panel.opacity = 0
        appear.restart()
    }
    function close() { visible = false }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: panel; property: "scale"; to: 1; duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        NumberAnimation { target: panel; property: "opacity"; to: 1; duration: Theme.fast }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.smoke
        MouseArea { anchors.fill: parent; hoverEnabled: true; onWheel: {} } // the page below can't be touched
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(dialog.dialogWidth, parent.width - 48)
        height: column.height
        radius: Theme.radiusOverlay
        color: Theme.dialog
        border.width: 1
        border.color: Qt.rgba(0, 0, 0, 0.45)
        focus: dialog.visible
        Keys.onEscapePressed: { dialog.close(); dialog.rejected() }

        Column {
            id: column
            width: parent.width
            Item {
                width: parent.width
                height: titleText.height + body.height + 24 + 24 + (dialog.title !== "" ? 12 : 0)
                Text {
                    id: titleText
                    x: 24
                    y: 24
                    width: parent.width - 48
                    text: dialog.title
                    visible: text !== ""
                    color: Theme.text
                    font.pixelSize: Theme.fontSubtitle
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                Item {
                    id: body
                    x: 24
                    y: dialog.title !== "" ? titleText.y + titleText.height + 12 : 24
                    width: parent.width - 48
                    height: childrenRect.height
                }
            }
            Rectangle {
                width: parent.width
                height: 80
                color: Theme.light ? "#f3f3f3" : "#202020"
                bottomLeftRadius: Theme.radiusOverlay
                bottomRightRadius: Theme.radiusOverlay
                Row {
                    anchors { fill: parent; margins: 24 }
                    spacing: 8
                    Button {
                        visible: dialog.primaryText !== ""
                        width: dialog.secondaryText !== "" ? (parent.width - 8) / 2 : parent.width
                        accent: true
                        usable: dialog.primaryEnabled
                        text: dialog.primaryText
                        onClicked: { dialog.close(); dialog.accepted() }
                    }
                    Button {
                        visible: dialog.secondaryText !== ""
                        width: dialog.primaryText !== "" ? (parent.width - 8) / 2 : parent.width
                        text: dialog.secondaryText
                        onClicked: { dialog.close(); dialog.rejected() }
                    }
                }
            }
        }
    }
}
