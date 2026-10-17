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
    // The panels with room for their shadow around them, cut where the
    // window meets the taskbar and the output's edge, and above on outputs
    // too short for it (docs/visual-coherence.md §6.9): the panels rest 12
    // units from both.
    readonly property real room: notificationsShadow.reach
    width: room + 360 + 12
    height: Math.min(room + motion.height + 12, Screen.height - Theme.taskbarHeight)
    color: "transparent"

    readonly property var history: Notifications.history
    property date month: new Date() // the month the calendar shows

    function open() {
        Menus.quickSettingsOpen = false
        // Reopened while leaving: it turns back from where it is, as it was.
        if (!motion.closing) {
            Notifications.collectPopups()
            month = new Date()
            Menus.placeOnTargetScreen(root)
        }
        visible = true
        Menus.notificationCenterOpen = true
        motion.open()
        panels.forceActiveFocus()
        updateBlur()
    }
    function close() {
        if (!motion.opened) {
            return
        }
        Menus.notificationCenterOpen = false
        motion.close()
    }

    Connections {
        target: Shell
        function onNotificationCenterRequested() { motion.opened ? root.close() : root.open() }
        function onQuickSettingsRequested() { root.close() }
    }

    onActiveChanged: {
        if (!active && motion.opened && !Menus.isOpen) {
            close()
        }
    }

    // The travel while the panels slide (PanelMotion.area), the two panels
    // at rest: not the shadows between them.
    function updateBlur() {
        Effects.setBlur(root, motion.moving ? [motion.visibleArea]
            : [notifications, calendar].map(p => Qt.rect(motion.x + p.x, motion.y + p.y, p.width, p.height)))
    }

    // The panels' small buttons: subtle, or the accent while on.
    component SmallButton: Button {
        subtle: !accent
        implicitWidth: text === "" ? 32 : implicitContentWidth + 20
        implicitHeight: 28
        leftPadding: 10
        rightPadding: 10
        font.pixelSize: Theme.fontCaption
    }

    ShadowRoom {
        panels: [notifications, calendar]
        onOutside: root.close()
    }

    // Both panels come 24 units from the right edge, and leave that way.
    PanelMotion {
        id: motion
        x: root.room
        y: root.height - 12 - height
        width: 360
        height: calendar.y + calendar.height
        offset: Qt.point(24, 0)
        onClosed: root.visible = false
        onVisibleAreaChanged: root.updateBlur()
        onMovingChanged: root.updateBlur()

        Item {
            id: panels
            anchors.fill: parent
            focus: true
            Keys.onEscapePressed: root.close()

            // Each shadow stops halfway between the panels: one doesn't fall on
            // the other, which is translucent.
            Shadow {
                id: notificationsShadow
                target: notifications
                below: 6
            }
            Shadow {
                target: calendar
                above: 6
            }

            // --- notifications ---
            Rectangle {
                id: notifications
                width: parent.width
                height: Math.min(Screen.height - Theme.taskbarHeight - calendar.height - 60, header.height + Math.max(list.contentHeight, 64) + 24)
                radius: Theme.radiusOverlay
                color: Theme.surface
                border.width: 1
                border.color: Theme.stroke

                Item {
                    id: header
                    x: 16
                    y: 10
                    width: parent.width - 32
                    height: 36
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Notifications")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    Row {
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        SmallButton {
                            icon.name: Notifications.doNotDisturb ? "notifications-disabled" : "notifications"
                            accent: Notifications.doNotDisturb
                            onClicked: Notifications.doNotDisturb = !Notifications.doNotDisturb
                        }
                        SmallButton {
                            visible: root.history.count > 0
                            text: qsTr("Clear all")
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

                    delegate: Fill {
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
                                            text: modelData.label
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
                            icon.name: "window-close"
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
                y: notifications.height + 12
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
                                icon.name: "go-up"
                                onClicked: root.month = new Date(root.month.getFullYear(), root.month.getMonth() - 1, 1)
                            }
                            SmallButton {
                                icon.name: "go-down"
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
                                Fill {
                                    anchors.centerIn: parent
                                    width: 32
                                    height: 32
                                    radius: 16
                                    color: parent.today ? Theme.accent : dayMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
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
}
