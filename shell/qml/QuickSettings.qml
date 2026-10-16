// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
import Vela.Audio
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
    // The panel with room for its shadow around it, cut where the window
    // meets the taskbar and the output's edge (docs/visual-coherence.md
    // §6.9): the panel rests 12 units from both.
    readonly property real room: shadow.reach
    width: room + 360 + 12
    height: room + d.roomHeight + 12
    color: "transparent"

    // "" the quick settings; "wifi", "sound" and "accessibility" their pages.
    property string page: ""

    function open() {
        Menus.notificationCenterOpen = false
        // Reopened while leaving: it turns back from where it is, as it was.
        if (!motion.closing) {
            page = ""
            d.settle()
            Menus.placeOnTargetScreen(root)
        }
        visible = true
        Menus.quickSettingsOpen = true
        motion.open()
        panel.forceActiveFocus()
        Effects.setBlur(root, [motion.visibleArea])
    }
    function close() {
        if (!motion.opened) {
            return
        }
        Menus.quickSettingsOpen = false
        motion.close()
    }
    function toggle() {
        if (motion.opened) {
            close()
        } else {
            open()
        }
    }
    // Win+Ctrl+V, "Open volume mixer": straight to the sound page.
    function toggleSound() {
        if (motion.opened && page === "sound") {
            close()
            return
        }
        if (!motion.opened) {
            open()
        }
        showPage("sound")
    }
    // Forward a page comes in from the right, back from the left.
    function showPage(name) {
        if (name === page) {
            return
        }
        pageSlideIn.from = (name === "" ? -24 : 24) * Theme.motion
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
    // The outputs (or inputs) for one app (the mixer), in a menu under its
    // button.
    function chooseAppDevice(app, button, input) {
        const choose = name => () => input ? Audio.setAppInput(app.key, name) : Audio.setAppOutput(app.key, name)
        const current = input ? app.input : app.output
        const entries = [{ text: input ? qsTr("Default input") : qsTr("Default output"), radio: true,
                           checked: current === "", action: choose("") },
                         { separator: true }]
        for (const device of input ? Audio.inputs : Audio.outputs) {
            entries.push({ text: device.description.replace(/&/g, "&&"), icon: device.icon, radio: true,
                           checked: current === device.name, action: choose(device.name) })
        }
        const p = screenPoint(button, 0, button.height + 4)
        Menus.open(entries, p.x, p.y, { minWidth: 240, screen: root.screen ? root.screen.name : "" })
    }
    // The system volume: the default output's.
    function setSystemVolume(value) {
        if (Audio.muted) Audio.setMuted("output", Audio.defaultOutput, false)
        Audio.setVolume("output", Audio.defaultOutput, value)
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
        if (!active && motion.opened && !Menus.isOpen) {
            close()
        }
    }

    // The panel's height follows its page (docs/visual-coherence.md §6.10):
    // between pages, and when a page grows or shrinks, it animates in
    // `normal`, at once without animation effects. The window grows to the
    // larger height before the panel animates and shrinks after it: two
    // configures per change, none per frame, and at rest the window is just
    // the panel and its shadow, so clicks above it reach what is below.
    readonly property real targetHeight: page === "" ? content.height + 24 + footer.height
        : (page === "wifi" ? wifiPage.finalHeight : page === "sound" ? soundPage.height : accessPage.height) + 32
    onTargetHeightChanged: d.fit()

    QtObject {
        id: d
        property real panelHeight: 0
        property real roomHeight: 0

        function settle() {
            heightAnimation.stop()
            panelHeight = root.targetHeight
            roomHeight = panelHeight
        }
        function fit() {
            if (!root.visible || Theme.motion === 0) {
                settle()
                return
            }
            if (heightAnimation.running && heightAnimation.to === root.targetHeight) {
                return
            }
            roomHeight = Math.max(roomHeight, panelHeight, root.targetHeight)
            heightAnimation.to = root.targetHeight
            heightAnimation.restart()
        }
    }
    Eased {
        id: heightAnimation
        target: d
        property: "panelHeight"
        onFinished: d.roomHeight = d.panelHeight // stopped by a new change: not yet
    }
    // A page comes in sliding and fading; the old one goes at once.
    ParallelAnimation {
        id: pageIn
        Eased { target: pages; property: "opacity"; from: 0; to: 1; duration: Theme.fast }
        Eased { id: pageSlideIn; target: pageSlide; property: "x"; to: 0 }
    }

    // One part of a tile (both halves of a split one): Qt's button, the
    // accent when on, every change of color faded (Fill).
    component TileButton: T.AbstractButton {
        id: half
        property bool on
        property bool roundLeft: true
        property bool roundRight: true
        height: parent.height
        z: visualFocus ? 1 : 0 // the ring above the other half
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        background: Fill {
            topLeftRadius: half.roundLeft ? Theme.radius : 0
            bottomLeftRadius: half.roundLeft ? Theme.radius : 0
            topRightRadius: half.roundRight ? Theme.radius : 0
            bottomRightRadius: half.roundRight ? Theme.radius : 0
            color: half.on ? (half.down ? Qt.tint(Theme.accent, Qt.rgba(1, 1, 1, 0.06))
                              : half.hovered ? Qt.tint(Theme.accent, Qt.rgba(1, 1, 1, 0.08)) : Theme.accent)
                 : (half.down ? Theme.controlPressed : half.hovered ? Theme.controlHover : Theme.control)
            FocusRing { visible: half.visualFocus }
        }
    }

    // A tile: on (accent color) or off, with the label below. `split`: an
    // arrow on the right opening its page (Wi-Fi); `menu`: the whole tile
    // opens the page (Accessibility). Turning on fades to the accent, hover
    // and press fade; a right click opens its Settings page.
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

        Item {
            id: face
            width: parent.width
            height: 48

            TileButton {
                id: mainButton
                width: tile.split ? parent.width - 36 : parent.width
                on: tile.checked
                roundRight: !tile.split
                enabled: tile.usable
                Accessible.name: tile.label
                Accessible.checkable: !tile.menu
                Accessible.checked: tile.checked
                onClicked: tile.menu ? tile.details() : tile.toggled()
            }
            TileButton {
                visible: tile.split
                x: parent.width - 36
                width: 36
                on: tile.checked
                roundLeft: false
                enabled: tile.usable
                Accessible.name: qsTr("%1 settings").arg(tile.label)
                onClicked: tile.details()
            }
            // Off: a thin border, which fades away as the tile turns on.
            Rectangle {
                anchors.fill: parent
                radius: Theme.radius
                color: "transparent"
                border.width: 1
                border.color: tile.checked ? Qt.rgba(Theme.stroke.r, Theme.stroke.g, Theme.stroke.b, 0) : Theme.stroke
                Behavior on border.color {
                    ColorAnimation { duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
                }
            }
            Fill {
                visible: tile.split
                x: parent.width - 36
                y: 10
                width: 1
                height: parent.height - 20
                radius: 0
                color: tile.checked ? Qt.rgba(1, 1, 1, 0.35) : Theme.stroke
            }
            Icon {
                x: (tile.split ? parent.width - 36 : parent.width) / 2 - width / 2
                anchors.verticalCenter: parent.verticalCenter
                name: tile.icon
                size: 18
                onAccent: tile.checked
            }
            Chevron {
                visible: tile.split || tile.menu
                x: tile.split ? parent.width - 21 : parent.width - 18
                anchors.verticalCenter: parent.verticalCenter
                color: tile.checked ? "white" : Theme.text
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                enabled: tile.usable
                onTapped: tile.contextMenu()
            }
        }
        Text {
            anchors { top: face.bottom; topMargin: 6; horizontalCenter: parent.horizontalCenter }
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
    // above the knob, like Windows. The wheel steps it like the volume keys
    // (`wheeled`), a step every notch.
    component SliderRow: Item {
        id: row
        property string icon
        property real value
        property bool muted: false
        // An app's icon (the mixer): muted, it fades and gets a crossed
        // speaker; the volume's icon is already the crossed one.
        property bool appIcon: false
        property bool details: false
        // What it plays now, 0-1 (docs/audio.md §7.1); below 0: no bar.
        property real level: -1
        signal moved(real value)
        signal wheeled(int direction)
        signal iconClicked()
        signal detailsClicked()
        width: parent.width
        height: 36

        Button {
            id: iconButton
            subtle: true
            width: 36
            height: 36
            icon.name: row.muted && row.appIcon ? "" : row.icon
            icon.width: 18
            icon.height: 18
            Accessible.name: row.muted ? qsTr("Unmute") : qsTr("Mute")
            onClicked: row.iconClicked()
            Icon {
                visible: row.muted && row.appIcon
                anchors.centerIn: parent
                name: row.icon
                size: 18
                opacity: 0.45
            }
            Icon {
                visible: row.muted && row.appIcon
                anchors { right: parent.right; bottom: parent.bottom; margins: 5 }
                name: "audio-volume-muted"
                size: 12
            }
        }
        // The level: a thin bar under the track, below the knob.
        Rectangle {
            visible: row.level >= 0
            x: slider.x + slider.trackStart
            y: slider.y + slider.height / 2 + 5
            width: slider.trackLength * Math.max(0, Math.min(1, row.level))
            height: 2
            radius: 1
            color: Theme.accentFill
            opacity: 0.6
        }
        // The track starts 12 after the icon: the knob's half-width earlier.
        Slider {
            id: slider
            anchors { left: iconButton.right; leftMargin: 2; right: detailsButton.left; rightMargin: -2; verticalCenter: parent.verticalCenter }
            value: row.value
            wheelEnabled: false
            fillColor: row.muted ? Theme.textSecondary : Theme.accentFill
            onMoved: row.moved(value)
        }
        Rectangle {
            visible: slider.pressed
            x: slider.x + slider.handle.x + slider.handle.width / 2 - width / 2
            y: slider.y + slider.handle.y - 6 - height
            width: valueText.implicitWidth + 16
            height: 26
            radius: Theme.radius
            color: Theme.popup
            border.width: 1
            border.color: Theme.stroke
            Text {
                id: valueText
                anchors.centerIn: parent
                text: Math.round(slider.value * 100)
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
        }
        WheelHandler {
            target: null
            // Touchpads add up their small steps.
            property real rest: 0
            onWheel: event => {
                rest += event.angleDelta.y
                while (Math.abs(rest) >= 120) {
                    row.wheeled(rest > 0 ? 1 : -1)
                    rest -= rest > 0 ? 120 : -120
                }
            }
        }
        Button {
            id: detailsButton
            anchors.right: parent.right
            visible: row.details
            subtle: true
            width: row.details ? 36 : 0
            height: 36
            Accessible.name: qsTr("Sound output")
            onClicked: row.detailsClicked()
            Chevron { anchors.centerIn: parent }
        }
    }

    // A page header: back, the title, and on the right what's needed.
    component PageHeader: Item {
        id: header
        property string title
        default property alias trailing: trailingSlot.data
        width: parent.width
        height: 36
        Button {
            id: back
            subtle: true
            width: 32
            height: 32
            anchors.verticalCenter: parent.verticalCenter
            icon.name: "go-previous"
            Accessible.name: qsTr("Back")
            onClicked: root.showPage("")
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

    // Where an app's sound goes or comes from, in the mixer: a small button
    // with an arrow, which opens the list.
    component DeviceChooser: T.AbstractButton {
        id: chooser
        anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
        width: visible ? Math.min(chooserMetrics.advanceWidth, soundColumn.width * 0.45) + 28 : 0
        height: 22
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        background: Fill {
            color: chooser.down ? Theme.subtlePressed : chooser.hovered ? Theme.subtleHover : Theme.subtleRest
            FocusRing { visible: chooser.visualFocus }
        }
        TextMetrics {
            id: chooserMetrics
            text: chooser.text
            font.pixelSize: Theme.fontCaption
        }
        contentItem: Item {
            Text {
                id: chooserLabel
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 28
                text: chooser.text
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
            Chevron {
                anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                down: true
                color: Theme.textSecondary
            }
        }
    }

    // A link at the bottom of a page ("More settings..."), like WinUI's
    // HyperlinkButton: its background fades in under the pointer. The text
    // stays aligned with the page.
    component Link: T.AbstractButton {
        id: link
        property string target
        x: -leftPadding
        leftPadding: 6
        rightPadding: 6
        topPadding: 4
        bottomPadding: 4
        implicitWidth: implicitContentWidth + leftPadding + rightPadding
        implicitHeight: implicitContentHeight + topPadding + bottomPadding
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        background: Fill {
            color: link.down ? Theme.subtlePressed : link.hovered ? Theme.subtleHover : Theme.subtleRest
            FocusRing { visible: link.visualFocus }
        }
        contentItem: Text {
            text: link.text
            color: Theme.link
            opacity: link.down ? 0.8 : 1
            font.pixelSize: Theme.fontBody
        }
        onClicked: root.openSettings(link.target)
    }

    ShadowRoom {
        panels: [panel]
        onOutside: root.close()
    }

    // It rises 48 units from behind the taskbar, where the window cuts it,
    // like Start, and leaves the same way.
    PanelMotion {
        id: motion
        x: root.room
        y: root.height - 12 - height
        width: 360
        height: d.panelHeight
        offset: Qt.point(0, 48)
        onClosed: {
            root.visible = false
            d.settle()
        }
        onVisibleAreaChanged: Effects.setBlur(root, [visibleArea]) // the travel while it moves, the panel at rest

        Shadow {
            id: shadow
            target: panel
        }

        Rectangle {
            id: panel
            anchors.fill: parent
            radius: Theme.radiusOverlay
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke
            focus: true
            clip: true
            Keys.onEscapePressed: {
                if (root.page !== "") {
                    root.showPage("")
                } else {
                    root.close()
                }
            }

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

                    SliderRow {
                        visible: Status.brightnessAvailable
                        icon: "brightness-high"
                        value: Status.brightness
                        onMoved: value => Status.setBrightness(value)
                        onWheeled: direction => Status.setBrightness(Math.max(0, Math.min(1, Status.brightness + direction * 0.02)))
                    }
                    // The arrow opens the sound page: outputs and the mixer.
                    SliderRow {
                        visible: Audio.available && Audio.defaultOutput !== ""
                        icon: Audio.iconName
                        value: Audio.volume
                        muted: Audio.muted
                        details: Audio.available
                        onMoved: value => root.setSystemVolume(value)
                        onWheeled: direction => Audio.stepVolume("output", Audio.defaultOutput, direction)
                        onIconClicked: Audio.setMuted("output", Audio.defaultOutput, !Audio.muted)
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
                    // Its height once the networks have finished opening or
                    // closing: the panel goes there at once, not frame by frame.
                    readonly property real finalHeight: height
                        + (networksView.visible ? Math.min(networks.finalHeight, 340) - networksView.height : 0)

                    PageHeader {
                        title: qsTr("Wi-Fi")
                        Switch {
                            showLabel: false
                            checked: Status.wifiEnabled
                            Accessible.name: qsTr("Wi-Fi")
                            onToggled: Status.setWifiEnabled(checked)
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
                        id: networksView
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
                            // Its height once every network has finished
                            // opening or closing.
                            readonly property real finalHeight: {
                                let h = height
                                for (let i = 0; i < networkRows.count; ++i) {
                                    const item = networkRows.itemAt(i)
                                    if (item) {
                                        h += item.fullHeight - item.height
                                    }
                                }
                                return h
                            }
                            Repeater {
                                id: networkRows
                                model: Network.wifiNetworks
                                // A network: a click opens it, with its
                                // password field and button, in `normal`.
                                delegate: ListItem {
                                    id: net
                                    required property var modelData
                                    readonly property bool open: wifiPage.expanded === modelData.ssid
                                    readonly property bool needsPassword: modelData.secure && !modelData.active
                                        && (!modelData.known || Network.passwordRequiredSsid === modelData.ssid)
                                    readonly property bool busy: wifiPage.connecting === modelData.ssid && Network.connectResult === ""
                                    readonly property real fullHeight: open ? 52 + actions.height + 12 : 52
                                    readonly property bool usable: !busy && (!needsPassword || wifiPage.password.length >= 8)
                                    width: networks.width
                                    height: fullHeight
                                    highlighted: open
                                    text: modelData.ssid
                                    Behavior on height { enabled: Theme.motion > 0; Eased {} }
                                    onClicked: {
                                        wifiPage.expanded = open ? "" : modelData.ssid
                                        wifiPage.password = ""
                                    }
                                    function activate() {
                                        if (!usable) return
                                        if (modelData.active) {
                                            Network.disconnectWifi(modelData.ssid)
                                        } else {
                                            wifiPage.connecting = modelData.ssid
                                            Network.connectWifi(modelData.ssid, needsPassword ? wifiPage.password : "")
                                        }
                                    }

                                    Icon {
                                        x: 12
                                        y: 16
                                        size: 20
                                        name: net.modelData.signal > 75 ? "network-wireless-signal-excellent"
                                            : net.modelData.signal > 50 ? "network-wireless-signal-good"
                                            : net.modelData.signal > 25 ? "network-wireless-signal-ok" : "network-wireless-signal-weak"
                                    }
                                    Column {
                                        x: 44
                                        y: (52 - height) / 2
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

                                    // Shown while open, and while it closes; a click there
                                    // doesn't close it.
                                    MouseArea {
                                        y: 52
                                        width: parent.width
                                        height: parent.height - 52
                                        clip: true
                                        Column {
                                            id: actions
                                            visible: net.open || net.height > 52
                                            x: 44
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
                                                    onAccepted: net.activate()
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
                                            Button {
                                                anchors.right: parent.right
                                                width: 120
                                                accent: !net.modelData.active
                                                usable: net.usable
                                                text: net.modelData.active ? qsTr("Disconnect") : net.needsPassword ? qsTr("Next") : qsTr("Connect")
                                                onClicked: net.activate()
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
                                model: Audio.outputs
                                delegate: ListItem {
                                    id: output
                                    required property var modelData
                                    width: soundColumn.width
                                    height: 44
                                    text: modelData.description
                                    highlighted: modelData.isDefault
                                    onClicked: Audio.setDefault("output", output.modelData.name)
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
                                    Icon {
                                        x: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: output.modelData.icon
                                        size: 18
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
                                        Icon {
                                            name: output.modelData.battery >= 90 ? "battery-full"
                                                : output.modelData.battery >= 40 ? "battery-good"
                                                : output.modelData.battery >= 15 ? "battery-low" : "battery-caution"
                                            size: 16
                                        }
                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: output.modelData.battery + "%"
                                            color: Theme.textSecondary
                                            font.pixelSize: Theme.fontCaption
                                        }
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
                                text: (Audio.outputs.find(o => o.isDefault) || { description: qsTr("Speakers") }).description
                                width: parent.width
                                leftPadding: 48
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontCaption
                                elide: Text.ElideRight
                            }
                            SliderRow {
                                icon: Audio.iconName
                                value: Audio.volume
                                muted: Audio.muted
                                onMoved: value => root.setSystemVolume(value)
                                onWheeled: direction => Audio.stepVolume("output", Audio.defaultOutput, direction)
                                onIconClicked: Audio.setMuted("output", Audio.defaultOutput, !Audio.muted)
                            }
                            Repeater {
                                model: Audio.apps
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
                                        DeviceChooser {
                                            id: appOutput
                                            visible: app.modelData.playing && (Audio.outputs.length > 1 || app.modelData.output !== "")
                                            text: app.modelData.output === "" ? qsTr("Default output") : app.modelData.outputDescription
                                            onClicked: root.chooseAppDevice(app.modelData, appOutput, false)
                                        }
                                    }
                                    LevelMeter {
                                        id: appLevel
                                        audio: Audio
                                        kind: "app"
                                        name: app.modelData.key
                                        active: root.visible && soundPage.visible && app.modelData.playing
                                    }
                                    SliderRow {
                                        visible: app.modelData.playing
                                        icon: app.modelData.icon
                                        appIcon: true
                                        value: app.modelData.volume
                                        level: appLevel.level
                                        muted: app.modelData.muted
                                        onMoved: value => Audio.setAppVolume(app.modelData.key, value)
                                        onWheeled: direction => Audio.stepAppVolume(app.modelData.key, direction)
                                        onIconClicked: Audio.setAppMuted(app.modelData.key, !app.modelData.muted)
                                    }
                                    // An app that records: where its sound comes
                                    // from.
                                    Item {
                                        visible: app.modelData.recording && (Audio.inputs.length > 1 || app.modelData.input !== "")
                                        width: parent.width
                                        height: visible ? 24 : 0
                                        Icon {
                                            x: 15
                                            anchors.verticalCenter: parent.verticalCenter
                                            name: app.modelData.playing ? "audio-input-microphone" : app.modelData.icon
                                            size: 16
                                        }
                                        DeviceChooser {
                                            id: appInput
                                            anchors.right: undefined
                                            x: 40
                                            text: app.modelData.input === "" ? qsTr("Default input") : app.modelData.inputDescription
                                            onClicked: root.chooseAppDevice(app.modelData, appInput, true)
                                        }
                                    }
                                }
                            }
                            Text {
                                visible: Audio.apps.length === 0
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
                        target: "volume-mixer" // Settings' Volume mixer, like Windows
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
                        delegate: ListItem {
                            id: option
                            required property var modelData
                            width: accessPage.width
                            height: 48
                            text: modelData.text
                            onClicked: Access[option.modelData.key] = !Access[option.modelData.key]
                            Icon {
                                x: 12
                                anchors.verticalCenter: parent.verticalCenter
                                name: option.modelData.icon
                                size: 18
                            }
                            Text {
                                x: 44
                                anchors.verticalCenter: parent.verticalCenter
                                text: option.modelData.text
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                            }
                            Switch {
                                anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                                showLabel: false
                                checked: Access[option.modelData.key]
                                Accessible.name: option.modelData.text
                                onToggled: Access[option.modelData.key] = checked
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
                opacity: pages.opacity // it comes back with its page
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 1 }
                height: 48
                color: Theme.footer
                bottomLeftRadius: Theme.radiusOverlay - 1
                bottomRightRadius: Theme.radiusOverlay - 1

                Row {
                    visible: Status.batteryPresent
                    anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                    spacing: 8
                    Icon {
                        name: Status.batteryCharging ? "battery-good-charging" : "battery-good"
                        size: 18
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: Status.batteryPercent + "%"
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
                    }
                }
                Button {
                    anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                    subtle: true
                    width: 36
                    height: 36
                    icon.name: "configure"
                    icon.width: 18
                    icon.height: 18
                    Accessible.name: qsTr("Settings")
                    onClicked: root.openSettings("settings")
                }
            }
        }
    }
}
