// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes
import Vela.Controls

// A notification's tile (NotificationPopups.qml): the app's icon and name,
// the summary, the body (links open), the app's actions, Close with the
// mouse over it. A click invokes the default action, or dismisses one
// without; a right click dismisses. It comes in from the right.
// Expressive: on surface container high (Theme.popup) with large corners
// and no outline, a critical one outlined in error, the summary in the
// emphasis weight; it slides in on the default spatial curve.
Rectangle {
    id: toast

    required property int notificationId
    required property string appName
    required property string icon
    required property string summary
    required property string body
    required property var actions
    required property bool hasDefaultAction
    required property bool critical

    // A notification's own actions: the app's ("default" for a click on
    // it, or one of its buttons), its dismissal, a link in its body.
    signal invoked(string key)
    signal dismissed()
    signal linkActivated(string link)

    readonly property bool expressive: Theme.expressive
    objectName: "toast"
    height: content.implicitHeight + 28
    radius: expressive ? Theme.shapeLarge : Theme.radiusOverlay
    color: Theme.popup
    border.width: expressive && !critical ? 0 : 1
    border.color: critical ? (expressive ? Theme.error : Qt.rgba(1, 0.35, 0.35, 0.6)) : Theme.stroke

    // It comes in from the right, like on Windows.
    transform: Translate { id: slide; x: 60 * Theme.motion }
    opacity: 0
    Component.onCompleted: appear.start()
    ParallelAnimation {
        id: appear
        Spatial { target: slide; property: "x"; to: 0; speed: "default"; rest: Theme.slow }
        Eased { target: toast; property: "opacity"; to: 1 }
    }

    // A passive right-button handler on the entire tile also works
    // over nested action buttons and links, without stealing their
    // existing left clicks or hover handling.
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: toast.dismissed()
    }

    MouseArea {
        id: toastMouse
        anchors.fill: parent
        hoverEnabled: true
        // Left click: invoke the app's default action, if any;
        // otherwise dismiss as before.
        onClicked: toast.hasDefaultAction ? toast.invoked("default") : toast.dismissed()
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
            textFormat: Text.PlainText // the spec's summary has no markup
            visible: text !== ""
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: Theme.weightEmphasis
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
            onLinkActivated: link => toast.linkActivated(link)
        }

        // The app's actions ("Reply", "Mark as read"...).
        Row {
            visible: toast.actions.length > 0
            topPadding: 4
            spacing: 8
            Repeater {
                model: toast.actions
                delegate: Button {
                    required property var modelData
                    implicitWidth: Math.max(80, implicitContentWidth + 24)
                    implicitHeight: 30
                    text: modelData.label
                    font.pixelSize: Theme.fontCaption
                    onClicked: toast.invoked(modelData.key)
                }
            }
        }
    }

    // Close: appears with the mouse over, at the top right.
    Button {
        id: closeButton
        anchors { right: parent.right; top: parent.top; margins: 8 }
        width: 24
        height: 24
        padding: 0
        subtle: true
        opacity: toastMouse.containsMouse || hovered ? 1 : 0
        Behavior on opacity { Eased { duration: Theme.fast } }
        Accessible.name: qsTr("Close")
        onClicked: toast.dismissed()
        contentItem: Item {
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
        }
    }
}
