// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes
import Vela.Controls

// Notifications, at the bottom right above the taskbar like on Windows 11. The
// window is as tall as the tiles: the rest of the output stays clickable.
Window {
    id: root
    width: 380
    height: Math.max(1, stack.implicitHeight + 24)
    visible: Notifications.count > 0
    color: "transparent"

    // The blurred background under the tiles: the whole window, the tiles give
    // the shape (outside is transparent).
    function updateBlur() {
        Effects.setBlur(root, [Qt.rect(0, 0, width, height)])
    }
    onWidthChanged: updateBlur()
    onHeightChanged: updateBlur()
    Component.onCompleted: updateBlur()

    // Mouse over: they don't expire while being read.
    HoverHandler {
        onHoveredChanged: Notifications.setHovered(hovered)
    }

    Column {
        id: stack
        x: 12
        y: 12
        width: parent.width - 24
        spacing: 8

        Repeater {
            model: Notifications

            delegate: Rectangle {
                id: toast

                required property int notificationId
                required property string appName
                required property string icon
                required property string summary
                required property string body
                required property var actions
                required property bool hasDefaultAction
                required property bool critical

                width: stack.width
                height: content.implicitHeight + 28
                radius: Theme.radiusOverlay
                color: Theme.popup
                border.width: 1
                border.color: critical ? Qt.rgba(1, 0.35, 0.35, 0.6) : Theme.stroke

                // It comes in from the right, like on Windows.
                transform: Translate { id: slide; x: 60 * Theme.motion }
                opacity: 0
                Component.onCompleted: appear.start()
                ParallelAnimation {
                    id: appear
                    NumberAnimation {
                        target: slide; property: "x"; to: 0
                        duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate
                    }
                    NumberAnimation {
                        target: toast; property: "opacity"; to: 1
                        duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate
                    }
                }

                MouseArea {
                    id: toastMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    // Click: opens the app (the "default" action), otherwise
                    // closes.
                    onClicked: toast.hasDefaultAction ? Notifications.invoke(toast.notificationId, "default")
                                                      : Notifications.dismiss(toast.notificationId)
                }

                Column {
                    id: content
                    x: 14
                    y: 14
                    width: parent.width - 28
                    spacing: 6

                    // App: icon and name, small, as a header.
                    Row {
                        spacing: 8
                        Image {
                            width: 16
                            height: 16
                            anchors.verticalCenter: parent.verticalCenter
                            source: toast.icon
                            sourceSize: Qt.size(width, height)
                            smooth: true
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: toast.appName
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                        }
                    }

                    Text {
                        width: parent.width - 20
                        text: toast.summary
                        visible: text !== ""
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: toast.body
                        visible: text !== ""
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                        textFormat: Text.StyledText // <b>, <i>, <a> from the spec
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        linkColor: Theme.accentFill
                        onLinkActivated: link => Qt.openUrlExternally(link)
                    }

                    // The app's actions ("Reply", "Mark as read"...).
                    Row {
                        visible: toast.actions.length > 0
                        topPadding: 4
                        spacing: 8
                        Repeater {
                            model: toast.actions
                            delegate: Rectangle {
                                required property var modelData
                                width: Math.max(80, actionLabel.implicitWidth + 24)
                                height: 30
                                radius: Theme.radius
                                color: actionMouse.pressed ? Theme.subtlePressed : actionMouse.containsMouse ? Theme.subtleHover : Theme.control
                                border.width: 1
                                border.color: Theme.stroke
                                Text {
                                    id: actionLabel
                                    anchors.centerIn: parent
                                    text: modelData.label
                                    color: Theme.text
                                    font.pixelSize: Theme.fontCaption
                                }
                                MouseArea {
                                    id: actionMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: Notifications.invoke(toast.notificationId, modelData.key)
                                }
                            }
                        }
                    }
                }

                // Close: appears with the mouse over, at the top right.
                Rectangle {
                    anchors { right: parent.right; top: parent.top; margins: 8 }
                    width: 24
                    height: 24
                    radius: Theme.radius
                    color: closeMouse.containsMouse ? Theme.subtleHover : "transparent"
                    opacity: toastMouse.containsMouse || closeMouse.containsMouse ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation { duration: Theme.fast }
                    }
                    Shape {
                        anchors.centerIn: parent
                        width: 10
                        height: 10
                        preferredRendererType: Shape.CurveRenderer
                        ShapePath {
                            strokeColor: Theme.text
                            strokeWidth: 1.2
                            capStyle: ShapePath.RoundCap
                            startX: 0; startY: 0
                            PathLine { x: 10; y: 10 }
                            PathMove { x: 10; y: 0 }
                            PathLine { x: 0; y: 10 }
                        }
                    }
                    MouseArea {
                        id: closeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: Notifications.dismiss(toast.notificationId)
                    }
                }
            }
        }
    }
}
