// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes
import Vela.Controls

// Quick settings (Win+A, or a click on the taskbar's system icons), like
// Windows 11: tiles to turn on and off (Wi-Fi, Bluetooth, airplane mode, power
// saving, night light...), brightness and volume, battery and Settings at the
// bottom. An acrylic panel at the bottom right. Some tiles have a page of
// their own: the Wi-Fi arrow lists the networks to choose one, Accessibility
// turns on the magnifier, color filters and sticky keys; the arrow next to
// the volume opens the sound page (outputs and the mixer, Win+Ctrl+V).
Window {
    id: root
    visible: false
    width: 384
    height: panel.height + 24
    color: "transparent"

    // "" the quick settings; "wifi", "sound" and "accessibility" their pages.
    property string page: ""
    // The mixer reads the system only while its page is on screen.
    readonly property bool soundShown: visible && page === "sound"
    onSoundShownChanged: Mixer.setActive(soundShown)

    function open() {
        Menus.notificationCenterOpen = false
        page = ""
        Menus.placeOnTargetScreen(root)
        visible = true
        Menus.quickSettingsOpen = true
        panel.opacity = 0
        slide.y = 12
        appear.restart()
        panel.forceActiveFocus()
        updateBlur()
    }
    function close() {
        visible = false
        Menus.quickSettingsOpen = false
    }
    function toggle() {
        if (visible) {
            close()
        } else {
            open()
        }
    }
    // Win+Ctrl+V, "Open volume mixer": straight to the sound page.
    function toggleSound() {
        if (visible && page === "sound") {
            close()
            return
        }
        if (!visible) {
            open()
        }
        showPage("sound")
    }
    function showPage(name) {
        page = name
        pageIn.restart()
        if (name === "wifi") {
            wifiPage.expanded = ""
            wifiPage.password = ""
            Network.scan() // cached networks appear immediately, rescan in background
        }
        panel.forceActiveFocus()
    }
    // At the bottom right, above the taskbar: from panel to output
    // coordinates.
    function screenPoint(item, x, y) {
        const p = item.mapToItem(null, x, y)
        return Qt.point(Screen.width - root.width + p.x, Screen.height - Theme.taskbarHeight - root.height + p.y)
    }
    // The outputs for one app (the mixer), in a menu under its button.
    function chooseAppOutput(app, button) {
        const choose = output => () => Mixer.setAppOutput(app.key, output)
        const entries = [{ text: qsTr("Default output"), radio: true, checked: app.output === "", action: choose("") },
                         { separator: true }]
        for (const output of Mixer.outputs) {
            entries.push({ text: output.description.replace(/&/g, "&&"), icon: output.icon, radio: true,
                           checked: app.output === output.name, action: choose(output.name) })
        }
        const p = screenPoint(button, 0, button.height + 4)
        Menus.open(entries, p.x, p.y, { minWidth: 240, screen: root.screen ? root.screen.name : "" })
    }
    function openSettings(name) {
        close()
        System.trigger(name)
    }

    Connections {
        target: Shell
        function onQuickSettingsRequested() { root.toggle() }
        function onSoundOutputRequested() { root.toggleSound() }
        function onNotificationCenterRequested() { root.close() }
    }

    // Back from a page: Alt+Left, the keyboard's Back key and the mouse's
    // side button (common/navigationbuttons.cpp), like the arrow at the top.
    Shortcut {
        sequences: [StandardKey.Back]
        enabled: root.page !== ""
        onActivated: root.showPage("")
    }

    onActiveChanged: {
        if (!active && visible && !Menus.isOpen) {
            close()
        }
    }

    function updateBlur() {
        Effects.setBlur(root, [Qt.rect(panel.x, panel.y, panel.width, panel.height)])
    }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: panel; property: "opacity"; to: 1; duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        NumberAnimation { target: slide; property: "y"; to: 0; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }
    // A page comes in from the right (back: like the others, it's short).
    ParallelAnimation {
        id: pageIn
        NumberAnimation { target: pages; property: "opacity"; from: 0; to: 1; duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
        NumberAnimation { target: pageSlide; property: "x"; from: 24; to: 0; duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }

    // A tile: on (accent color) or off, with the label below. `split`: an
    // arrow on the right opening its page (Wi-Fi); `menu`: the whole tile
    // opens the page (Accessibility).
    component Tile: Item {
        id: tile
        property string icon
        property string label
        property bool checked
        property bool usable: true
        property bool split: false
        property bool menu: false
        signal toggled()
        signal details()
        signal contextMenu()
        width: 104
        height: 82
        opacity: usable ? 1 : 0.45

        Rectangle {
            id: button
            width: parent.width
            height: 48
            radius: Theme.radius
            color: tile.checked ? Theme.accent : Theme.control
            border.width: tile.checked ? 0 : 1
            border.color: Theme.stroke
            clip: true

            // The two halves (or the whole tile), each with its hover.
            Rectangle {
                x: 0
                width: tile.split ? parent.width - 36 : parent.width
                height: parent.height
                radius: parent.radius
                color: tile.checked ? "white" : Theme.overlay
                opacity: mainMouse.pressed ? 0.06 : mainMouse.containsMouse && tile.usable ? 0.08 : 0
            }
            Rectangle {
                visible: tile.split
                x: parent.width - 36
                width: 36
                height: parent.height
                radius: parent.radius
                color: tile.checked ? "white" : Theme.overlay
                opacity: detailsMouse.pressed ? 0.06 : detailsMouse.containsMouse && tile.usable ? 0.08 : 0
            }
            Rectangle {
                visible: tile.split
                x: parent.width - 36
                y: 10
                width: 1
                height: parent.height - 20
                color: tile.checked ? Qt.rgba(1, 1, 1, 0.35) : Theme.stroke
            }
            Image {
                x: (tile.split ? parent.width - 36 : parent.width) / 2 - width / 2
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                source: (tile.checked ? Theme.iconsOnAccent : Theme.icons) + encodeURIComponent(tile.icon)
                sourceSize: Qt.size(width, height)
            }
            // The ">" arrow.
            Shape {
                visible: tile.split || tile.menu
                x: tile.split ? parent.width - 21 : parent.width - 18
                anchors.verticalCenter: parent.verticalCenter
                width: 6
                height: 10
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: tile.checked ? "white" : Theme.text
                    strokeWidth: 1.2
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin
                    startX: 1; startY: 1
                    PathLine { x: 5; y: 5 }
                    PathLine { x: 1; y: 9 }
                }
            }
            MouseArea {
                id: mainMouse
                width: tile.split ? parent.width - 36 : parent.width
                height: parent.height
                hoverEnabled: true
                enabled: tile.usable
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: mouse => {
                    if (mouse.button === Qt.RightButton) {
                        tile.contextMenu()
                    } else if (tile.menu) {
                        tile.details()
                    } else {
                        tile.toggled()
                    }
                }
            }
            MouseArea {
                id: detailsMouse
                visible: tile.split
                x: parent.width - 36
                width: 36
                height: parent.height
                hoverEnabled: true
                enabled: tile.usable
                onClicked: tile.details()
            }
        }
        Text {
            anchors { top: button.bottom; topMargin: 6; horizontalCenter: parent.horizontalCenter }
            width: parent.width
            text: tile.label
            color: Theme.text
            font.pixelSize: Theme.fontCaption
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
    }

    // A slider (brightness, volume, an app's volume) with its icon on the
    // left: a click on it mutes. `details`: an arrow on the right opening a
    // page (the volume's: the sound page). While it's dragged the number shows
    // above the knob, like Windows.
    component Slider: Item {
        id: slider
        property string icon
        property real value
        property bool muted: false
        // An app's icon (the mixer): muted, it fades and gets a crossed
        // speaker; the volume's icon is already the crossed one.
        property bool appIcon: false
        property bool details: false
        signal moved(real value)
        signal wheeled(int direction)
        signal iconClicked()
        signal detailsClicked()
        readonly property real shown: Math.max(0, Math.min(1, value))
        width: parent.width
        height: 36

        Item {
            id: iconButton
            width: 36
            height: 36
            Rectangle {
                anchors.fill: parent
                radius: Theme.radius
                color: Theme.subtleHover
                opacity: iconMouse.containsMouse ? 1 : 0
            }
            Image {
                anchors.centerIn: parent
                width: 18
                height: 18
                opacity: slider.muted && slider.appIcon ? 0.45 : 1
                source: Theme.icons + encodeURIComponent(slider.icon)
                sourceSize: Qt.size(width, height)
            }
            Image {
                visible: slider.muted && slider.appIcon
                anchors { right: parent.right; bottom: parent.bottom; margins: 5 }
                width: 12
                height: 12
                source: Theme.icons + "audio-volume-muted"
                sourceSize: Qt.size(width, height)
            }
            MouseArea {
                id: iconMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: slider.iconClicked()
            }
        }
        Item {
            id: track
            anchors { left: iconButton.right; leftMargin: 12; right: detailsButton.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
            height: 20
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: 2
                color: Theme.stroke
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width * slider.shown
                height: 4
                radius: 2
                color: slider.muted ? Theme.textSecondary : Theme.accent
            }
            Rectangle {
                id: knob
                x: parent.width * slider.shown - width / 2
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                radius: 9
                color: Theme.popup
                border.width: 1
                border.color: Theme.stroke
                Rectangle {
                    anchors.centerIn: parent
                    width: trackMouse.pressed ? 8 : 10
                    height: width
                    radius: width / 2
                    color: slider.muted ? Theme.textSecondary : Theme.accent
                }
            }
            Rectangle {
                visible: trackMouse.pressed
                anchors { bottom: knob.top; bottomMargin: 6; horizontalCenter: knob.horizontalCenter }
                width: valueText.implicitWidth + 16
                height: 26
                radius: Theme.radius
                color: Theme.popup
                border.width: 1
                border.color: Theme.stroke
                Text {
                    id: valueText
                    anchors.centerIn: parent
                    text: Math.round(slider.shown * 100)
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                }
            }
            MouseArea {
                id: trackMouse
                anchors { fill: parent; margins: -8 }
                function update(x) { slider.moved(Math.max(0, Math.min(1, (x - 8) / track.width))) }
                onPressed: mouse => update(mouse.x)
                onPositionChanged: mouse => { if (pressed) update(mouse.x) }
                // A step every notch (120), touchpads add up their small ones.
                property real wheelRest: 0
                onWheel: wheel => {
                    wheelRest += wheel.angleDelta.y
                    while (Math.abs(wheelRest) >= 120) {
                        slider.wheeled(wheelRest > 0 ? 1 : -1)
                        wheelRest -= wheelRest > 0 ? 120 : -120
                    }
                }
            }
        }
        Item {
            id: detailsButton
            anchors.right: parent.right
            width: slider.details ? 36 : 0
            height: 36
            visible: slider.details
            Rectangle {
                anchors.fill: parent
                radius: Theme.radius
                color: Theme.subtleHover
                opacity: detailsMouse.containsMouse ? 1 : 0
            }
            Shape {
                anchors.centerIn: parent
                width: 6
                height: 10
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: Theme.text
                    strokeWidth: 1.2
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin
                    startX: 1; startY: 1
                    PathLine { x: 5; y: 5 }
                    PathLine { x: 1; y: 9 }
                }
            }
            MouseArea {
                id: detailsMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: slider.detailsClicked()
            }
        }
    }

    // A toggle switch like Windows 11's.
    component Switch: Item {
        id: toggle
        property bool checked
        signal toggled()
        width: 40
        height: 20
        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: toggle.checked ? Theme.accent : "transparent"
            border.width: toggle.checked ? 0 : 1
            border.color: Theme.textSecondary
            Rectangle {
                x: toggle.checked ? parent.width - width - 4 : 4
                anchors.verticalCenter: parent.verticalCenter
                width: switchMouse.containsMouse ? 14 : 12
                height: width
                radius: width / 2
                color: toggle.checked ? "white" : Theme.textSecondary
                Behavior on x { NumberAnimation { duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate } }
            }
        }
        MouseArea {
            id: switchMouse
            anchors { fill: parent; margins: -4 }
            hoverEnabled: true
            onClicked: toggle.toggled()
        }
    }

    // A page header: back, the title, and on the right what's needed.
    component PageHeader: Item {
        id: header
        property string title
        default property alias trailing: trailingSlot.data
        width: parent.width
        height: 36
        Item {
            id: back
            width: 32
            height: 32
            anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                anchors.fill: parent
                radius: Theme.radius
                color: Theme.subtleHover
                opacity: backMouse.containsMouse ? 1 : 0
            }
            Image {
                anchors.centerIn: parent
                width: 16
                height: 16
                source: Theme.icons + "go-previous"
                sourceSize: Qt.size(width, height)
            }
            MouseArea {
                id: backMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.showPage("")
            }
        }
        Text {
            anchors { left: back.right; leftMargin: 8; verticalCenter: parent.verticalCenter }
            text: header.title
            color: Theme.text
            font.pixelSize: Theme.fontBody + 2
            font.weight: Font.DemiBold
        }
        Item {
            id: trailingSlot
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            width: childrenRect.width
            height: childrenRect.height
        }
    }

    // A link at the bottom of a page ("More settings...").
    component Link: Text {
        id: link
        property string target
        color: linkMouse.containsMouse ? Theme.accentFill : Theme.accent
        font.pixelSize: Theme.fontBody
        font.underline: linkMouse.containsMouse
        MouseArea {
            id: linkMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.openSettings(link.target)
        }
    }

    Rectangle {
        id: panel
        x: 12
        y: 12
        width: root.width - 24
        height: root.page === "" ? content.height + 24 + footer.height
            : (root.page === "wifi" ? wifiPage.height : root.page === "sound" ? soundPage.height : accessPage.height) + 32
        radius: Theme.radiusOverlay
        color: Theme.surface
        border.width: 1
        border.color: Theme.stroke
        transform: Translate { id: slide }
        focus: true
        clip: true
        Keys.onEscapePressed: {
            if (root.page !== "") {
                root.showPage("")
            } else {
                root.close()
            }
        }
        onHeightChanged: root.updateBlur()

        Item {
            id: pages
            anchors.fill: parent
            transform: Translate { id: pageSlide }

            // ---------------------------------------------------------- main --
            Column {
                id: content
                visible: root.page === ""
                x: 16
                y: 16
                width: parent.width - 32
                spacing: 16

                Grid {
                    columns: 3
                    columnSpacing: 8
                    rowSpacing: 4

                    Tile {
                        visible: Status.wifiAvailable
                        icon: Status.wifiEnabled ? "network-wireless" : "network-wireless-disconnected"
                        label: Status.networkWireless && Status.networkName !== "" ? Status.networkName : qsTr("Wi-Fi")
                        checked: Status.wifiEnabled
                        split: Network.available
                        onToggled: Status.setWifiEnabled(!Status.wifiEnabled)
                        onDetails: root.showPage("wifi")
                        onContextMenu: root.openSettings("network")
                    }
                    // Without Wi-Fi (a wired desktop): the network as it is,
                    // for information.
                    Tile {
                        visible: !Status.wifiAvailable
                        icon: Status.networkIconName
                        label: Status.networkConnected ? (Status.networkName !== "" ? Status.networkName : qsTr("Network")) : qsTr("Not connected")
                        checked: Status.networkConnected
                        onToggled: root.openSettings("network")
                        onContextMenu: root.openSettings("network")
                    }
                    Tile {
                        visible: Status.bluetoothAvailable
                        icon: Status.bluetoothEnabled ? "network-bluetooth" : "network-bluetooth-inactive"
                        label: qsTr("Bluetooth")
                        checked: Status.bluetoothEnabled
                        split: true
                        onToggled: Status.setBluetoothEnabled(!Status.bluetoothEnabled)
                        onDetails: root.openSettings("bluetooth-settings")
                        onContextMenu: root.openSettings("bluetooth-settings")
                    }
                    Tile {
                        visible: Status.wifiAvailable || Status.bluetoothAvailable
                        icon: "network-flightmode-on"
                        label: qsTr("Airplane mode")
                        checked: Status.airplane
                        onToggled: Status.setAirplaneMode(!Status.airplane)
                    }
                    Tile {
                        visible: Status.powerSaverAvailable
                        icon: "battery-profile-powersave"
                        label: qsTr("Energy saver")
                        checked: Status.powerSaver
                        onToggled: Status.setPowerSaver(!Status.powerSaver)
                        onContextMenu: root.openSettings("power")
                    }
                    Tile {
                        icon: "redshift-status-on"
                        label: qsTr("Night light")
                        checked: Access.nightLight
                        onToggled: Access.nightLight = !Access.nightLight
                        onContextMenu: root.openSettings("night-light-settings")
                    }
                    Tile {
                        icon: "preferences-desktop-accessibility"
                        label: qsTr("Accessibility")
                        checked: Access.magnifier || Access.colorFilter || Access.stickyKeys
                        menu: true
                        onDetails: root.showPage("accessibility")
                        onContextMenu: root.openSettings("accessibility-settings")
                    }
                }

                Slider {
                    visible: Status.brightnessAvailable
                    icon: "brightness-high"
                    value: Status.brightness
                    onMoved: value => Status.setBrightness(value)
                    onWheeled: direction => Status.setBrightness(Math.max(0, Math.min(1, Status.brightness + direction * 0.02)))
                }
                // The arrow opens the sound page: outputs and the mixer.
                Slider {
                    visible: Status.volumeAvailable
                    icon: Status.volumeIconName
                    value: Status.volume
                    muted: Status.muted
                    details: Mixer.available
                    onMoved: value => { if (Status.muted) Status.setMuted(false); Status.setVolume(value) }
                    onWheeled: direction => Status.stepVolume(direction)
                    onIconClicked: Status.setMuted(!Status.muted)
                    onDetailsClicked: root.showPage("sound")
                }
            }

            // --------------------------------------------- Wi-Fi networks --
            Column {
                id: wifiPage
                visible: root.page === "wifi"
                x: 16
                y: 16
                width: parent.width - 32
                spacing: 8
                property string expanded: "" // the open network
                property string connecting: "" // the network being connected to
                property string password: ""

                PageHeader {
                    title: qsTr("Wi-Fi")
                    Switch {
                        checked: Status.wifiEnabled
                        onToggled: Status.setWifiEnabled(!Status.wifiEnabled)
                    }
                }
                Text {
                    visible: !Status.wifiEnabled || Network.wifiNetworks.length === 0
                    width: parent.width
                    topPadding: 8
                    bottomPadding: 8
                    text: !Status.wifiEnabled ? qsTr("Wi-Fi is turned off.")
                        : Network.scanning ? qsTr("Looking for networks...") : qsTr("No networks found.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                }
                Flickable {
                    visible: Status.wifiEnabled && Network.wifiNetworks.length > 0
                    width: parent.width
                    height: Math.min(contentHeight, 340)
                    contentHeight: networks.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    Column {
                        id: networks
                        width: parent.width
                        spacing: 2
                        Repeater {
                            model: Network.wifiNetworks
                            delegate: Rectangle {
                                id: net
                                required property var modelData
                                readonly property bool open: wifiPage.expanded === modelData.ssid
                                readonly property bool needsPassword: modelData.secure && !modelData.active
                                    && (!modelData.known || Network.passwordRequiredSsid === modelData.ssid)
                                readonly property bool busy: wifiPage.connecting === modelData.ssid && Network.connectResult === ""
                                width: networks.width
                                height: open ? row.height + actions.height + 12 : row.height
                                radius: Theme.radius
                                color: open ? Theme.control : netMouse.containsMouse ? Theme.subtleHover : "transparent"
                                clip: true

                                Item {
                                    id: row
                                    width: parent.width
                                    height: 52
                                    Image {
                                        x: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 20
                                        height: 20
                                        source: Theme.icons + (net.modelData.signal > 75 ? "network-wireless-signal-excellent"
                                            : net.modelData.signal > 50 ? "network-wireless-signal-good"
                                            : net.modelData.signal > 25 ? "network-wireless-signal-ok" : "network-wireless-signal-weak")
                                        sourceSize: Qt.size(width, height)
                                    }
                                    Column {
                                        x: 44
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width - 56
                                        Text {
                                            width: parent.width
                                            text: net.modelData.ssid
                                            color: Theme.text
                                            font.pixelSize: Theme.fontBody
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: net.busy ? qsTr("Connecting...")
                                                : net.modelData.active ? (net.modelData.secure ? qsTr("Connected, secured") : qsTr("Connected"))
                                                : net.modelData.secure ? qsTr("Secured") : qsTr("Open")
                                            color: Theme.textSecondary
                                            font.pixelSize: Theme.fontCaption
                                        }
                                    }
                                    MouseArea {
                                        id: netMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: {
                                            wifiPage.expanded = net.open ? "" : net.modelData.ssid
                                            wifiPage.password = ""
                                        }
                                    }
                                }

                                Column {
                                    id: actions
                                    visible: net.open
                                    x: 44
                                    y: row.height
                                    width: parent.width - 56
                                    spacing: 8

                                    // Ask for a key only on first join or if a
                                    // saved connection fails authentication.
                                    Rectangle {
                                        visible: net.needsPassword
                                        width: parent.width
                                        height: 32
                                        radius: Theme.radius
                                        color: Theme.control
                                        border.width: 1
                                        border.color: passwordField.activeFocus ? Theme.accent : Theme.stroke
                                        MenuTextField {
                                            id: passwordField
                                            anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
                                            verticalAlignment: TextInput.AlignVCenter
                                            echoMode: TextInput.Password
                                            text: wifiPage.password
                                            onTextEdited: wifiPage.password = text
                                            onAccepted: connectButton.activate()
                                            mapToScreen: (x, y) => root.screenPoint(passwordField, x, y)
                                        }
                                        Text {
                                            anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                                            visible: passwordField.text === ""
                                            text: qsTr("Enter the network security key")
                                            color: Theme.textSecondary
                                            font.pixelSize: Theme.fontCaption
                                        }
                                    }
                                    Text {
                                        visible: wifiPage.connecting === net.modelData.ssid && Network.connectResult !== "" && Network.connectResult !== "ok"
                                        width: parent.width
                                        text: Network.connectResult
                                        color: Theme.light ? "#c42b1c" : "#ff99a4"
                                        font.pixelSize: Theme.fontCaption
                                        wrapMode: Text.WordWrap
                                    }
                                    Rectangle {
                                        id: connectButton
                                        anchors.right: parent.right
                                        width: 120
                                        height: 32
                                        radius: Theme.radius
                                        readonly property bool primary: !net.modelData.active
                                        readonly property bool usable: !net.busy && (!net.needsPassword || wifiPage.password.length >= 8)
                                        opacity: usable ? 1 : 0.5
                                        color: primary ? (buttonMouse.containsMouse ? Qt.lighter(Theme.accent, 1.1) : Theme.accent)
                                            : buttonMouse.containsMouse ? Theme.subtleHover : Theme.control
                                        border.width: primary ? 0 : 1
                                        border.color: Theme.stroke
                                        function activate() {
                                            if (!usable) return
                                            if (net.modelData.active) {
                                                Network.disconnectWifi(net.modelData.ssid)
                                            } else {
                                                wifiPage.connecting = net.modelData.ssid
                                                Network.connectWifi(net.modelData.ssid, net.needsPassword ? wifiPage.password : "")
                                            }
                                        }
                                        Text {
                                            anchors.centerIn: parent
                                            text: net.modelData.active ? qsTr("Disconnect") : net.needsPassword ? qsTr("Next") : qsTr("Connect")
                                            color: connectButton.primary ? "white" : Theme.text
                                            font.pixelSize: Theme.fontBody
                                        }
                                        MouseArea {
                                            id: buttonMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onClicked: connectButton.activate()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                Item { width: 1; height: 4 }
                Link {
                    text: qsTr("More Wi-Fi settings")
                    target: "network"
                }
            }

            // ------------------------------------------------------- sound --
            // Like Windows 11's: where the sound goes, then the volume of the
            // output and of each app playing.
            Column {
                id: soundPage
                visible: root.page === "sound"
                x: 16
                y: 16
                width: parent.width - 32
                spacing: 8

                PageHeader { title: qsTr("Sound output") }
                Flickable {
                    width: parent.width
                    height: Math.min(contentHeight, 440)
                    contentHeight: soundColumn.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    Column {
                        id: soundColumn
                        width: parent.width
                        spacing: 2

                        Repeater {
                            model: Mixer.outputs
                            delegate: Rectangle {
                                id: output
                                required property var modelData
                                width: soundColumn.width
                                height: 44
                                radius: Theme.radius
                                color: outputMouse.containsMouse ? Theme.subtleHover : output.modelData.isDefault ? Theme.control : "transparent"
                                // The chosen one: the accent line on the left,
                                // like a selected item on Windows.
                                Rectangle {
                                    visible: output.modelData.isDefault
                                    x: 0
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 3
                                    height: 16
                                    radius: 1.5
                                    color: Theme.accent
                                }
                                Image {
                                    x: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 18
                                    height: 18
                                    source: Theme.icons + encodeURIComponent(output.modelData.icon)
                                    sourceSize: Qt.size(width, height)
                                }
                                Text {
                                    x: 44
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 56 - (battery.visible ? battery.width + 8 : 0)
                                    text: output.modelData.description
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBody
                                    elide: Text.ElideRight
                                }
                                // A Bluetooth headset's charge, when it
                                // reports it.
                                Row {
                                    id: battery
                                    visible: output.modelData.battery >= 0
                                    anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                                    spacing: 4
                                    Image {
                                        width: 16
                                        height: 16
                                        source: Theme.icons + (output.modelData.battery >= 90 ? "battery-full"
                                            : output.modelData.battery >= 40 ? "battery-good"
                                            : output.modelData.battery >= 15 ? "battery-low" : "battery-caution")
                                        sourceSize: Qt.size(width, height)
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: output.modelData.battery + "%"
                                        color: Theme.textSecondary
                                        font.pixelSize: Theme.fontCaption
                                    }
                                }
                                MouseArea {
                                    id: outputMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: Mixer.setDefaultOutput(output.modelData.name)
                                }
                            }
                        }

                        Item { width: 1; height: 12 }
                        Text {
                            text: qsTr("Volume mixer")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            font.weight: Font.DemiBold
                            bottomPadding: 4
                        }
                        // The output's volume first, then the apps'.
                        Text {
                            text: (Mixer.outputs.find(o => o.isDefault) || { description: qsTr("Speakers") }).description
                            width: parent.width
                            leftPadding: 48
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideRight
                        }
                        Slider {
                            icon: Status.volumeIconName
                            value: Status.volume
                            muted: Status.muted
                            onMoved: value => { if (Status.muted) Status.setMuted(false); Status.setVolume(value) }
                            onWheeled: direction => Status.stepVolume(direction)
                            onIconClicked: Status.setMuted(!Status.muted)
                        }
                        Repeater {
                            model: Mixer.apps
                            delegate: Column {
                                id: app
                                required property var modelData
                                width: soundColumn.width
                                topPadding: 6
                                // The name, and on the right where its sound
                                // goes: a click chooses another output.
                                Item {
                                    width: parent.width
                                    height: 20
                                    Text {
                                        x: 48
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width - 48 - appOutput.width - 12
                                        text: app.modelData.name
                                        color: Theme.textSecondary
                                        font.pixelSize: Theme.fontCaption
                                        elide: Text.ElideRight
                                    }
                                    Rectangle {
                                        id: appOutput
                                        visible: Mixer.outputs.length > 1 || app.modelData.output !== ""
                                        anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
                                        width: visible ? Math.min(outputLabel.implicitWidth, soundColumn.width * 0.45) + 28 : 0
                                        height: 22
                                        radius: Theme.radius
                                        color: outputChooser.containsMouse ? Theme.subtleHover : "transparent"
                                        Text {
                                            id: outputLabel
                                            x: 8
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: parent.width - 28
                                            text: app.modelData.output === "" ? qsTr("Default output") : app.modelData.outputDescription
                                            color: Theme.textSecondary
                                            font.pixelSize: Theme.fontCaption
                                            elide: Text.ElideRight
                                        }
                                        // The "v" arrow.
                                        Shape {
                                            anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                                            width: 9
                                            height: 5
                                            preferredRendererType: Shape.CurveRenderer
                                            ShapePath {
                                                strokeColor: Theme.textSecondary
                                                strokeWidth: 1.2
                                                fillColor: "transparent"
                                                capStyle: ShapePath.RoundCap
                                                joinStyle: ShapePath.RoundJoin
                                                startX: 1; startY: 1
                                                PathLine { x: 4.5; y: 4.5 }
                                                PathLine { x: 8; y: 1 }
                                            }
                                        }
                                        MouseArea {
                                            id: outputChooser
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onClicked: root.chooseAppOutput(app.modelData, appOutput)
                                        }
                                    }
                                }
                                Slider {
                                    icon: app.modelData.icon
                                    appIcon: true
                                    value: app.modelData.volume
                                    muted: app.modelData.muted
                                    onMoved: value => Mixer.setAppVolume(app.modelData.key, value)
                                    onWheeled: direction => Mixer.stepAppVolume(app.modelData.key, direction)
                                    onIconClicked: Mixer.setAppMuted(app.modelData.key, !app.modelData.muted)
                                }
                            }
                        }
                        Text {
                            visible: Mixer.apps.length === 0
                            width: parent.width
                            topPadding: 8
                            leftPadding: 48
                            text: qsTr("No apps are playing sound.")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.WordWrap
                        }
                    }
                }
                Item { width: 1; height: 4 }
                Link {
                    text: qsTr("More volume settings")
                    target: "sound-settings"
                }
            }

            // ------------------------------------------------ accessibility --
            Column {
                id: accessPage
                visible: root.page === "accessibility"
                x: 16
                y: 16
                width: parent.width - 32
                spacing: 4

                PageHeader { title: qsTr("Accessibility") }
                Item { width: 1; height: 4 }
                Repeater {
                    model: [
                        { icon: "zoom-in", text: qsTr("Magnifier"), key: "magnifier" },
                        { icon: "preferences-desktop-color", text: qsTr("Color filters"), key: "colorFilter" },
                        { icon: "input-keyboard", text: qsTr("Sticky keys"), key: "stickyKeys" }
                    ]
                    delegate: Rectangle {
                        id: option
                        required property var modelData
                        width: accessPage.width
                        height: 48
                        radius: Theme.radius
                        color: optionMouse.containsMouse ? Theme.subtleHover : "transparent"
                        Image {
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: 18
                            height: 18
                            source: Theme.icons + option.modelData.icon
                            sourceSize: Qt.size(width, height)
                        }
                        Text {
                            x: 44
                            anchors.verticalCenter: parent.verticalCenter
                            text: option.modelData.text
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                        }
                        MouseArea {
                            id: optionMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: Access[option.modelData.key] = !Access[option.modelData.key]
                        }
                        Switch {
                            anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                            checked: Access[option.modelData.key]
                            onToggled: Access[option.modelData.key] = !Access[option.modelData.key]
                        }
                    }
                }
                Item { width: 1; height: 8 }
                Link {
                    text: qsTr("More accessibility settings")
                    target: "accessibility-settings"
                }
            }
        }

        // At the bottom: battery on the left, Settings on the right.
        Rectangle {
            id: footer
            visible: root.page === ""
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 1 }
            height: 48
            color: Theme.footer
            bottomLeftRadius: Theme.radiusOverlay - 1
            bottomRightRadius: Theme.radiusOverlay - 1

            Row {
                visible: Status.batteryPresent
                anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                spacing: 8
                Image {
                    width: 18
                    height: 18
                    source: Theme.icons + encodeURIComponent(Status.batteryCharging ? "battery-good-charging" : "battery-good")
                    sourceSize: Qt.size(width, height)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Status.batteryPercent + "%"
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                }
            }
            Item {
                anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                width: 36
                height: 36
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius
                    color: Theme.subtleHover
                    opacity: settingsMouse.containsMouse ? 1 : 0
                }
                Image {
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    source: Theme.icons + "configure"
                    sourceSize: Qt.size(width, height)
                }
                MouseArea {
                    id: settingsMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.openSettings("settings")
                }
            }
        }
    }
}
