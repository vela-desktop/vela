// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The notification center and calendar (Win+N, or a click on the clock), like
// Windows 11: past notifications on top, grouped by app, with "Do not disturb"
// and "Clear all"; the month's calendar below. Two acrylic panels at the
// bottom right.
Window {
    id: root
    visible: false
    width: 384
    height: column.height + 24
    color: "transparent"

    readonly property var history: Notifications.history
    property date month: new Date() // the month the calendar shows

    function open() {
        Menus.quickSettingsOpen = false
        Notifications.collectPopups()
        month = new Date()
        Menus.placeOnTargetScreen(root)
        visible = true
        Menus.notificationCenterOpen = true
        column.opacity = 0
        slide.x = 24
        appear.restart()
        column.forceActiveFocus()
        updateBlur()
    }
    function close() {
        visible = false
        Menus.notificationCenterOpen = false
    }

    Connections {
        target: Shell
        function onNotificationCenterRequested() { root.visible ? root.close() : root.open() }
        function onQuickSettingsRequested() { root.close() }
    }

    onActiveChanged: {
        if (!active && visible && !Menus.isOpen) {
            close()
        }
    }

    function updateBlur() {
        Effects.setBlur(root, [Qt.rect(column.x, column.y + notifications.y, column.width, notifications.height),
            Qt.rect(column.x, column.y + calendar.y, column.width, calendar.height)])
    }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: column; property: "opacity"; to: 1; duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        NumberAnimation { target: slide; property: "x"; to: 0; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }

    component SmallButton: Item {
        id: smallButton
        property string label
        property string icon
        property bool checked: false
        signal clicked()
        width: icon !== "" ? 32 : buttonText.implicitWidth + 20
        height: 28
        Rectangle {
            anchors.fill: parent
            radius: Theme.radius
            color: smallButton.checked ? Theme.accent : Theme.subtleHover
            opacity: smallButton.checked || buttonMouse.containsMouse ? 1 : 0
        }
        Text {
            id: buttonText
            visible: smallButton.label !== ""
            anchors.centerIn: parent
            text: smallButton.label
            color: Theme.text
            font.pixelSize: Theme.fontCaption
        }
        Image {
            visible: smallButton.icon !== ""
            anchors.centerIn: parent
            width: 16
            height: 16
            source: smallButton.icon !== "" ? Theme.icons + encodeURIComponent(smallButton.icon) : ""
            sourceSize: Qt.size(width, height)
        }
        MouseArea {
            id: buttonMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: smallButton.clicked()
        }
    }

    Column {
        id: column
        x: 12
        y: 12
        width: root.width - 24
        spacing: 12
        transform: Translate { id: slide }
        focus: true
        Keys.onEscapePressed: root.close()
        onHeightChanged: root.updateBlur()

        // --- notifications ---
        Rectangle {
            id: notifications
            width: parent.width
            height: Math.min(Screen.height - Theme.taskbarHeight - calendar.height - 60, header.height + Math.max(list.contentHeight, 64) + 24)
            radius: Theme.radiusOverlay
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke
            onHeightChanged: root.updateBlur()

            Item {
                id: header
                x: 16
                y: 10
                width: parent.width - 32
                height: 36
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        text: qsTr("Notifications")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    Text {
                        visible: Notifications.captureSilenced
                        text: qsTr("Silenced while your screen is shared")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                    }
                }
                Row {
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    spacing: 4
                    SmallButton {
                        icon: Notifications.silenced ? "notifications-disabled" : "notifications"
                        checked: Notifications.doNotDisturb
                        onClicked: Notifications.doNotDisturb = !Notifications.doNotDisturb
                    }
                    SmallButton {
                        visible: root.history.count > 0
                        label: qsTr("Clear all")
                        onClicked: Notifications.clearHistory()
                    }
                }
            }

            ListView {
                id: list
                anchors { top: header.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; margins: 8; topMargin: 4 }
                clip: true
                spacing: 6
                model: root.history
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

                delegate: Rectangle {
                    id: item
                    required property int notificationId
                    required property string icon
                    required property string summary
                    required property string body
                    required property var actions
                    required property bool hasDefaultAction
                    required property date time
                    width: list.width
                    height: itemContent.height + 20
                    radius: Theme.radius
                    color: itemHover.hovered ? Theme.subtleHover : Theme.control

                    HoverHandler { id: itemHover }
                    MouseArea {
                        id: itemMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (item.hasDefaultAction) {
                                Notifications.invokeFromHistory(item.notificationId, "default")
                                root.close()
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
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBody
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Text {
                                    id: timeText
                                    anchors.right: parent.right
                                    anchors.rightMargin: 24
                                    text: Qt.formatTime(item.time, "HH:mm")
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
                                        label: modelData.label
                                        onClicked: Notifications.invokeFromHistory(item.notificationId, modelData.key)
                                    }
                                }
                            }
                        }
                    }
                    // Close (on hover), like on Windows.
                    SmallButton {
                        anchors { right: parent.right; top: parent.top; margins: 6 }
                        visible: itemHover.hovered
                        icon: "window-close"
                        onClicked: Notifications.dismissFromHistory(item.notificationId)
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
        }

        // --- calendar ---
        Rectangle {
            id: calendar
            width: parent.width
            height: calendarContent.height + 32
            radius: Theme.radiusOverlay
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke

            Column {
                id: calendarContent
                x: 16
                y: 16
                width: parent.width - 32
                spacing: 12

                // Today, in full ("Saturday, October 4"), like Windows.
                Text {
                    text: {
                        const s = Qt.locale().toString(new Date(), "dddd d MMMM")
                        return s.charAt(0).toUpperCase() + s.slice(1)
                    }
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                }

                Item {
                    width: parent.width
                    height: 28
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: {
                            const s = Qt.locale().toString(root.month, "MMMM yyyy")
                            return s.charAt(0).toUpperCase() + s.slice(1)
                        }
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Row {
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        SmallButton {
                            icon: "go-up"
                            onClicked: root.month = new Date(root.month.getFullYear(), root.month.getMonth() - 1, 1)
                        }
                        SmallButton {
                            icon: "go-down"
                            onClicked: root.month = new Date(root.month.getFullYear(), root.month.getMonth() + 1, 1)
                        }
                    }
                }

                // The grid: the week starts on Monday.
                Grid {
                    id: days
                    columns: 7
                    readonly property real cell: (parent.width) / 7
                    readonly property int offset: (new Date(root.month.getFullYear(), root.month.getMonth(), 1).getDay() + 6) % 7
                    readonly property int length: new Date(root.month.getFullYear(), root.month.getMonth() + 1, 0).getDate()

                    Repeater {
                        model: ["lu", "ma", "me", "gi", "ve", "sa", "do"]
                        delegate: Text {
                            required property string modelData
                            width: days.cell
                            height: 28
                            text: modelData
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Repeater {
                        model: 42
                        delegate: Item {
                            required property int index
                            readonly property date day: new Date(root.month.getFullYear(), root.month.getMonth(), index - days.offset + 1)
                            readonly property bool inMonth: day.getMonth() === root.month.getMonth()
                            readonly property bool today: {
                                const now = new Date()
                                return day.getFullYear() === now.getFullYear() && day.getMonth() === now.getMonth()
                                    && day.getDate() === now.getDate()
                            }
                            width: days.cell
                            height: 36
                            Rectangle {
                                anchors.centerIn: parent
                                width: 32
                                height: 32
                                radius: 16
                                color: parent.today ? Theme.accent : Theme.subtleHover
                                opacity: parent.today || dayMouse.containsMouse ? 1 : 0
                            }
                            Text {
                                anchors.centerIn: parent
                                text: parent.day.getDate()
                                color: parent.today ? "white" : parent.inMonth ? Theme.text : Theme.textSecondary
                                opacity: parent.inMonth || parent.today ? 1 : 0.5
                                font.pixelSize: Theme.fontCaption
                            }
                            MouseArea {
                                id: dayMouse
                                anchors.fill: parent
                                hoverEnabled: true
                            }
                        }
                    }
                }
            }
        }
    }
}
