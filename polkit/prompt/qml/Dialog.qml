// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Effects
import QtQuick.Window
import Vela.Controls

// User Account Control (docs/polkit-agent.md §6.2): over the screen's veil,
// who is asking and what for, the identity and the password, Yes and No.
//
// Every Text is plain text: names, messages and PAM's words come from apps,
// .desktop files and the system, and markup in them must not change how the
// dialog looks.
Window {
    id: window
    color: "transparent"
    title: qsTr("User Account Control")

    readonly property bool canAnswer: Request.asking && !Request.checking && !Request.unavailable
                                      && (field.text !== "" || Request.echo)
    readonly property var selectedIdentity: {
        for (const identity of Request.identities) {
            if (identity.uid === Request.selectedUid)
                return identity
        }
        return null
    }
    property bool capsLock: false
    property bool showDetails: false

    function answer() {
        if (!canAnswer)
            return
        Request.respond(field.text)
        field.text = ""
    }

    // Scrolls the dialog's content so that `item` is in view.
    function reveal(item) {
        const y = item.mapToItem(column, 0, 0).y
        if (y < scroller.contentY)
            scroller.contentY = Math.max(0, y - 16)
        else if (y + item.height > scroller.contentY + scroller.height)
            scroller.contentY = Math.min(scroller.contentHeight - scroller.height, y + item.height + 16 - scroller.height)
    }

    function updateBlur() {
        Effects.setBlur(window, [Qt.rect(0, 0, width, height)])
    }
    onWidthChanged: updateBlur()
    onHeightChanged: updateBlur()
    Component.onCompleted: updateBlur()

    Connections {
        target: Request
        function onAsked() {
            field.text = ""
            field.forceActiveFocus()
            Qt.callLater(window.reveal, fieldRow)
        }
        function onRetried() {
            field.text = ""
            shake.restart()
        }
    }

    // --- the veil ---
    Rectangle {
        anchors.fill: parent
        color: Theme.veil
        opacity: 0
        Component.onCompleted: opacity = Qt.binding(() => Request.closing ? 0 : 1)
        Behavior on opacity {
            NumberAnimation {
                duration: Request.closing ? Theme.normal : Theme.slow
                easing.type: Easing.BezierSpline
                easing.bezierCurve: Theme.decelerate
            }
        }
    }
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
    }

    // --- the dialog ---
    FocusScope {
        id: scope
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: Request.cancel()

        RectangularShadow {
            anchors.fill: panel
            radius: panel.radius
            blur: 48
            offset: Qt.vector2d(0, 16)
            color: Qt.rgba(0, 0, 0, 0.45)
            opacity: panel.opacity
            scale: panel.scale
        }

        Rectangle {
            id: panel
            objectName: "panel"
            anchors.centerIn: parent
            width: Math.min(456, parent.width - 32)
            height: Math.min(column.height + footer.height, parent.height - 32)
            radius: Theme.radiusOverlay
            color: Theme.dialog
            border.width: 1
            border.color: Theme.dialogStroke
            opacity: 0
            scale: 1 + 0.05 * Theme.motion
            Component.onCompleted: {
                opacity = Qt.binding(() => Request.closing ? 0 : 1)
                scale = Qt.binding(() => Request.closing ? 1 - 0.02 * Theme.motion : 1)
            }
            Behavior on opacity { NumberAnimation { duration: Request.closing ? Theme.fast : Theme.normal } }
            Behavior on scale {
                NumberAnimation { duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
            }

            Accessible.role: Accessible.Dialog
            Accessible.name: window.title

            // On a short output the content above the buttons scrolls.
            Flickable {
                id: scroller
                anchors { top: parent.top; left: parent.left; right: parent.right; bottom: footer.top }
                contentHeight: column.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height
                // Sizes arrive after the first layout: keep the password in view.
                onHeightChanged: if (field.activeFocus) Qt.callLater(window.reveal, fieldRow)
                onContentHeightChanged: if (field.activeFocus) Qt.callLater(window.reveal, fieldRow)
                Column {
                    id: column
                    width: scroller.width

                    // Title and question.
                    Item {
                        width: parent.width
                        height: header.height + 48
                        Column {
                            id: header
                            x: 24
                            y: 24
                            width: parent.width - 48
                            spacing: 12
                            Text {
                                textFormat: Text.PlainText
                                width: parent.width
                                text: window.title
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontCaption
                            }
                            Text {
                                textFormat: Text.PlainText
                                width: parent.width
                                text: qsTr("Do you want to allow this app to make changes to your device?")
                                color: Theme.text
                                font.pixelSize: Theme.fontSubtitle
                                font.weight: Font.DemiBold
                                wrapMode: Text.Wrap
                            }
                        }
                    }

                    // Who is asking, and what for.
                    Item {
                        width: parent.width
                        height: app.height + 8
                        Row {
                            id: app
                            x: 24
                            width: parent.width - 48
                            spacing: 16
                            Image {
                                width: 40
                                height: 40
                                sourceSize: Qt.size(width, height)
                                source: {
                                    const icon = Request.appIcon !== "" ? Request.appIcon
                                               : Request.iconName !== "" ? Request.iconName : "application-x-executable"
                                    return Theme.icons + encodeURIComponent(icon)
                                }
                            }
                            Column {
                                width: parent.width - 56
                                spacing: 4
                                Text {
                                    textFormat: Text.PlainText
                                    objectName: "appName"
                                    width: parent.width
                                    text: Request.appName !== "" ? Request.appName : qsTr("Unknown program")
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBodyLarge
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Text {
                                    textFormat: Text.PlainText
                                    width: parent.width
                                    text: Request.message
                                    visible: text !== ""
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontBody
                                    wrapMode: Text.Wrap
                                }
                                // "Show more details"
                                Text {
                                    textFormat: Text.PlainText
                                    id: detailsLink
                                    topPadding: 4
                                    text: window.showDetails ? qsTr("Hide details") : qsTr("Show more details")
                                    color: Theme.link
                                    font.pixelSize: Theme.fontBody
                                    font.underline: detailsMouse.containsMouse || activeFocus
                                    activeFocusOnTab: true
                                    Accessible.role: Accessible.Link
                                    Accessible.name: text
                                    Keys.onSpacePressed: window.showDetails = !window.showDetails
                                    Keys.onReturnPressed: window.showDetails = !window.showDetails
                                    MouseArea {
                                        id: detailsMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: window.showDetails = !window.showDetails
                                    }
                                }
                                Flickable {
                                    id: details
                                    visible: window.showDetails
                                    width: parent.width
                                    height: Math.min(detailsColumn.height, 140)
                                    contentHeight: detailsColumn.height
                                    clip: true
                                    boundsBehavior: Flickable.StopAtBounds
                                    Column {
                                        id: detailsColumn
                                        width: details.width
                                        spacing: 2
                                        Repeater {
                                            model: [{ key: qsTr("Action"), value: Request.actionId }].concat(Request.details)
                                            Text {
                                                required property var modelData
                                                width: parent.width
                                                text: modelData.key + ": " + modelData.value
                                                textFormat: Text.PlainText
                                                color: Theme.textSecondary
                                                font.pixelSize: Theme.fontCaption
                                                wrapMode: Text.WrapAnywhere
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        x: 24
                        width: parent.width - 48
                        height: 1
                        color: Theme.divider
                    }

                    // Identity and password.
                    Item {
                        width: parent.width
                        height: credentials.height + 40
                        Column {
                            id: credentials
                            x: 24
                            y: 20
                            width: parent.width - 48
                            spacing: 12

                            Text {
                                textFormat: Text.PlainText
                                width: parent.width
                                text: window.selectedIdentity && window.selectedIdentity.uid === Request.currentUid
                                      ? qsTr("To continue, enter your password.")
                                      : qsTr("To continue, enter an administrator's password.")
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                wrapMode: Text.Wrap
                            }

                            // One identity, or the choice among several.
                            Flickable {
                                id: identityList
                                width: parent.width
                                // Three and a half tiles at most: a long list scrolls.
                                height: Math.min(identityColumn.height, 56 * 3.5 + 4 * 3)
                                contentHeight: identityColumn.height
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                Column {
                                    id: identityColumn
                                    width: identityList.width
                                    spacing: 4
                                    Repeater {
                                        model: Request.chooseIdentity ? Request.identities
                                               : (window.selectedIdentity ? [window.selectedIdentity] : [])
                                        delegate: Rectangle {
                                            id: tile
                                            required property var modelData
                                            readonly property bool selected: modelData.uid === Request.selectedUid
                                            objectName: "identity-" + modelData.uid
                                            width: parent.width
                                            height: 56
                                            radius: Theme.radius
                                            color: !Request.chooseIdentity ? "transparent"
                                                 : selected ? Theme.controlPressed
                                                 : tileMouse.containsMouse ? Theme.subtleHover : "transparent"
                                            border.width: Request.chooseIdentity && selected ? 1 : 0
                                            border.color: Theme.accentFill
                                            activeFocusOnTab: Request.chooseIdentity
                                            Accessible.role: Request.chooseIdentity ? Accessible.RadioButton : Accessible.StaticText
                                            Accessible.name: modelData.name
                                            Accessible.checked: selected
                                            Keys.onSpacePressed: Request.selectIdentity(modelData.uid)

                                            Row {
                                                anchors.verticalCenter: parent.verticalCenter
                                                x: Request.chooseIdentity ? 8 : 0
                                                spacing: 12
                                                Item {
                                                    width: 40
                                                    height: 40
                                                    Rectangle {
                                                        id: avatarMask
                                                        anchors.fill: parent
                                                        radius: width / 2
                                                        color: Theme.accentFill
                                                        layer.enabled: true
                                                        visible: avatar.status !== Image.Ready
                                                        Text {
                                                            textFormat: Text.PlainText
                                                            anchors.centerIn: parent
                                                            text: (tile.modelData.name || "?").charAt(0).toUpperCase()
                                                            color: Theme.accentText
                                                            font.pixelSize: Theme.fontBodyLarge
                                                            font.weight: Font.DemiBold
                                                        }
                                                    }
                                                    Image {
                                                        id: avatar
                                                        anchors.fill: parent
                                                        visible: false
                                                        source: tile.modelData.avatar ? "file://" + tile.modelData.avatar : ""
                                                        sourceSize: Qt.size(width, height)
                                                        fillMode: Image.PreserveAspectCrop
                                                    }
                                                    MultiEffect {
                                                        anchors.fill: parent
                                                        source: avatar
                                                        visible: avatar.status === Image.Ready
                                                        maskEnabled: true
                                                        maskSource: avatarMask
                                                    }
                                                }
                                                Column {
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    Text {
                                                        textFormat: Text.PlainText
                                                        text: tile.modelData.name
                                                        color: Theme.text
                                                        font.pixelSize: Theme.fontBody
                                                        font.weight: Font.DemiBold
                                                    }
                                                    Text {
                                                        textFormat: Text.PlainText
                                                        text: tile.modelData.login
                                                        visible: text !== tile.modelData.name
                                                        color: Theme.textSecondary
                                                        font.pixelSize: Theme.fontCaption
                                                    }
                                                }
                                            }
                                            MouseArea {
                                                id: tileMouse
                                                anchors.fill: parent
                                                enabled: Request.chooseIdentity
                                                hoverEnabled: true
                                                onClicked: Request.selectIdentity(tile.modelData.uid)
                                            }
                                        }
                                    }
                                }
                            }

                            // PAM's question: usually "Password:", and then the
                            // field's placeholder is enough.
                            Text {
                                textFormat: Text.PlainText
                                readonly property bool plainPassword: /^\s*(password|passwor[dt]|parola d'ordine)\s*:?\s*$/i.test(Request.promptText)
                                width: parent.width
                                visible: Request.promptText !== "" && (Request.echo || !plainPassword)
                                text: Request.promptText
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                wrapMode: Text.Wrap
                            }

                            Item {
                                id: fieldRow
                                width: parent.width
                                height: field.height
                                TextBox {
                                    id: field
                                    objectName: "password"
                                    width: parent.width
                                    password: !Request.echo
                                    enabled: !Request.unavailable && !Request.closing
                                    readOnly: Request.checking
                                    placeholderText: Request.echo ? "" : qsTr("Password")
                                    focus: true
                                    Accessible.name: Request.promptText !== "" ? Request.promptText : qsTr("Password")
                                    onAccepted: window.answer()
                                onActiveFocusChanged: if (activeFocus) Qt.callLater(window.reveal, fieldRow)
                                    // Caps Lock: an uppercase letter without Shift (or the
                                    // opposite). Qt doesn't tell the key's state.
                                    Keys.onPressed: event => {
                                        const t = event.text
                                        if (t.length === 1 && t.toUpperCase() !== t.toLowerCase()) {
                                            const upper = t === t.toUpperCase()
                                            const shift = (event.modifiers & Qt.ShiftModifier) !== 0
                                            window.capsLock = upper !== shift
                                        } else if (event.key === Qt.Key_CapsLock) {
                                            window.capsLock = !window.capsLock
                                        }
                                        event.accepted = false
                                    }
                                }
                                SequentialAnimation {
                                    id: shake
                                    loops: 1
                                    NumberAnimation { target: field; property: "x"; to: -10 * Theme.motion; duration: 50 }
                                    NumberAnimation { target: field; property: "x"; to: 10 * Theme.motion; duration: 70 }
                                    NumberAnimation { target: field; property: "x"; to: -6 * Theme.motion; duration: 60 }
                                    NumberAnimation { target: field; property: "x"; to: 4 * Theme.motion; duration: 50 }
                                    NumberAnimation { target: field; property: "x"; to: 0; duration: 40 }
                                }
                                // While PAM checks: a line running under the field.
                                Item {
                                    anchors { left: field.left; right: field.right; bottom: field.bottom }
                                    height: 2
                                    clip: true
                                    visible: Request.checking
                                    Rectangle {
                                        id: runner
                                        width: parent.width / 3
                                        height: parent.height
                                        radius: 1
                                        color: Theme.accentFill
                                        NumberAnimation on x {
                                            running: Request.checking
                                            from: -runner.width
                                            to: fieldRow.width
                                            duration: 1100
                                            loops: Animation.Infinite
                                        }
                                    }
                                }
                            }

                            Text {
                                textFormat: Text.PlainText
                                width: parent.width
                                visible: window.capsLock && !Request.echo && field.activeFocus
                                text: qsTr("Caps Lock is on.")
                                color: Theme.caution
                                font.pixelSize: Theme.fontCaption
                            }
                            Text {
                                textFormat: Text.PlainText
                                objectName: "info"
                                width: parent.width
                                visible: text !== ""
                                text: Request.infoText
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                            }
                            Text {
                                textFormat: Text.PlainText
                                objectName: "error"
                                width: parent.width
                                visible: text !== ""
                                text: Request.errorText
                                color: Theme.critical
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                                Accessible.role: Accessible.AlertMessage
                            }
                        }
                    }
                }
            }

            // Yes and No, in the band at the bottom: always in view.
            Rectangle {
                id: footer
                anchors.bottom: parent.bottom
                width: parent.width
                height: 80
                color: Theme.dialogFooter
                bottomLeftRadius: Theme.radiusOverlay
                bottomRightRadius: Theme.radiusOverlay
                Row {
                    anchors { fill: parent; margins: 24 }
                    spacing: 8
                    Button {
                        width: (parent.width - 8) / 2
                        accent: true
                        text: qsTr("Yes")
                        usable: window.canAnswer
                        onClicked: window.answer()
                    }
                    Button {
                        width: (parent.width - 8) / 2
                        text: qsTr("No")
                        onClicked: Request.cancel()
                    }
                }
            }
        }
    }
}
