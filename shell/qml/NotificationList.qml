// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The notification center's past notifications (NotificationCenter.qml),
// grouped by app under its name: each one's icon, summary, time, body and
// actions; a click invokes its default action, a right click or Close
// (with the mouse over it) dismisses it.
ListView {
    id: list
    // The notification center's style (Styles.notifications); the tests
    // pass their own.
    property QtObject style: Styles.notifications
    // How times show (Config.timeFormat).
    property string timeFormat: Config.timeFormat
    // What the cards ask for: an action of a notification ("default" for a
    // click on it), its dismissal.
    signal invoked(int notificationId, string key)
    signal dismissed(int notificationId)
    clip: true
    // Expressive: an app's notifications 2 apart, one container (M3's
    // segmented list, as Settings' card groups).
    spacing: style.expressive ? 2 : 6
    boundsBehavior: Flickable.StopAtBounds
    section.property: "appName"
    section.delegate: Text {
        required property string section
        width: list.width
        leftPadding: 8
        topPadding: 6
        bottomPadding: 2
        text: section
        color: Theme.textSecondary
        font.pixelSize: Theme.fontCaption
    }

    delegate: Fill {
        id: item
        objectName: "notificationCard"
        required property int notificationId
        required property string icon
        required property string summary
        required property string body
        required property var actions
        required property bool hasDefaultAction
        required property date time
        // Expressive: the first and the last of an app's run take the
        // outer corners (Styles' cardRadius), those between M3's extra
        // small ones; the neighbors' app from the model (the list's
        // sections).
        readonly property bool expressive: list.style.expressive
        readonly property bool first: ListView.previousSection !== ListView.section
        readonly property bool last: ListView.nextSection !== ListView.section
        readonly property real outer: list.style.cardRadius
        readonly property real inner: Math.min(outer, Theme.shapeExtraSmall)
        width: list.width
        height: itemContent.height + 20
        radius: list.style.cardRadius
        // undefined: the default look keeps `radius` alone.
        topLeftRadius: expressive ? (first ? outer : inner) : undefined
        topRightRadius: expressive ? (first ? outer : inner) : undefined
        bottomLeftRadius: expressive ? (last ? outer : inner) : undefined
        bottomRightRadius: expressive ? (last ? outer : inner) : undefined
        color: itemHover.hovered ? (expressive ? Theme.controlHover : Theme.subtleHover) : Theme.control
        Accessible.role: Accessible.ListItem
        Accessible.name: summary
        Accessible.description: body

        HoverHandler { id: itemHover }
        // Passive right-click handling works across child controls
        // without changing their left-button actions.
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: list.dismissed(item.notificationId)
        }
        MouseArea {
            id: itemMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: {
                if (item.hasDefaultAction) {
                    list.invoked(item.notificationId, "default")
                }
            }
        }
        Row {
            id: itemContent
            x: 12
            y: 10
            width: parent.width - 24
            spacing: 12
            Image {
                width: 28
                height: 28
                source: item.icon
                sourceSize: Qt.size(width, height)
                fillMode: Image.PreserveAspectFit
            }
            Column {
                width: parent.width - 40
                spacing: 2
                Item {
                    width: parent.width
                    height: summaryText.height
                    Text {
                        id: summaryText
                        width: parent.width - timeText.width - 30
                        text: item.summary
                        textFormat: Text.PlainText // the spec's summary has no markup
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: item.expressive ? Theme.weightEmphasis : Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        id: timeText
                        anchors.right: parent.right
                        anchors.rightMargin: 24
                        text: Qt.formatTime(item.time, list.timeFormat)
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                    }
                }
                Text {
                    visible: item.body !== ""
                    width: parent.width
                    text: item.body
                    textFormat: Text.StyledText
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                    maximumLineCount: 4
                    elide: Text.ElideRight
                }
                Row {
                    visible: item.actions.length > 0
                    spacing: 6
                    topPadding: 6
                    Repeater {
                        model: item.actions
                        delegate: SmallButton {
                            required property var modelData
                            text: modelData.label
                            onClicked: list.invoked(item.notificationId, modelData.key)
                        }
                    }
                }
            }
        }
        // Close (on hover), like on Windows.
        SmallButton {
            anchors { right: parent.right; top: parent.top; margins: 6 }
            visible: itemHover.hovered
            icon.name: "window-close"
            onClicked: list.dismissed(item.notificationId)
        }
    }

    Text {
        anchors.centerIn: parent
        visible: list.count === 0
        text: qsTr("No new notifications")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontBody
    }
}
