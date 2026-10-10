// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// Network & internet: how the PC is connected, the Wi-Fi networks around and
// the properties of each connection (addresses, DNS), like Windows 11.
Page {
    id: page
    Component.onCompleted: {
        Network.refresh()
        if (Status.wifiAvailable && Status.wifiEnabled) {
            Network.scan()
        }
    }
    readonly property var wifiDevice: Network.devices.find(d => d.type === "wifi")
    readonly property var ethernetDevices: Network.devices.filter(d => d.type === "ethernet")
    property string expanded: "" // the device whose properties are open

    function stateText(d) {
        if (d.state.indexOf("connected") === 0 && d.state.indexOf("disconnected") < 0) return qsTr("Connected")
        if (d.state === "unavailable") return d.type === "ethernet" ? qsTr("Cable unplugged") : qsTr("Not available")
        if (d.state.indexOf("connecting") >= 0) return qsTr("Connecting...")
        return qsTr("Disconnected")
    }

    // At the top: the status, large.
    Item {
        width: parent.width
        height: 96
        Image {
            id: statusIcon
            anchors.verticalCenter: parent.verticalCenter
            width: 64
            height: 64
            source: Theme.icons + encodeURIComponent(Status.networkIconName)
            sourceSize: Qt.size(width, height)
        }
        Column {
            anchors { left: statusIcon.right; leftMargin: 16; verticalCenter: parent.verticalCenter }
            Text {
                text: Status.networkConnected ? (Status.networkName !== "" ? Status.networkName : qsTr("Connected")) : qsTr("Not connected")
                color: Theme.text
                font.pixelSize: Theme.fontSubtitle
                font.weight: Font.DemiBold
            }
            Text {
                text: Status.networkConnected ? qsTr("Connected to the Internet") : qsTr("No connection")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }
        }
    }

    Card {
        visible: !Network.available
        icon: "dialog-warning"
        title: qsTr("NetworkManager isn't available")
        description: qsTr("nmcli is needed to see and change connections.")
    }

    // --- Wi-Fi ---
    Card {
        visible: Status.wifiAvailable
        icon: "network-wireless"
        title: qsTr("Wi-Fi")
        description: Status.wifiEnabled ? (page.wifiDevice && page.wifiDevice.connection !== "" ? qsTr("Connected to ") + page.wifiDevice.connection : qsTr("Not connected")) : qsTr("Off")
        trailing: Toggle {
            checked: Status.wifiEnabled
            onToggled: on => {
                Status.setWifiEnabled(on)
                if (on) scanLater.start()
            }
        }
        Timer { id: scanLater; interval: 2000; onTriggered: Network.scan() }
    }
    Card {
        visible: Status.wifiAvailable && Status.wifiEnabled
        icon: "view-refresh"
        title: qsTr("Available networks")
        description: Network.scanning ? qsTr("Searching...") : Network.wifiNetworks.length + qsTr(" networks")
        trailing: Button {
            text: qsTr("Refresh")
            usable: !Network.scanning
            onClicked: Network.scan()
        }
        contentItem: Column {
            width: parent.width
            spacing: 2
            Repeater {
                model: Network.wifiNetworks
                delegate: Rectangle {
                    id: net
                    required property var modelData
                    width: parent.width
                    height: 48
                    radius: Theme.radius
                    color: netMouse.containsMouse ? Theme.subtleHover : "transparent"
                    Image {
                        x: 4
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20
                        height: 20
                        source: Theme.icons + (net.modelData.signal > 75 ? "network-wireless-signal-excellent"
                            : net.modelData.signal > 50 ? "network-wireless-signal-good"
                            : net.modelData.signal > 25 ? "network-wireless-signal-ok" : "network-wireless-signal-weak")
                        sourceSize: Qt.size(width, height)
                    }
                    Column {
                        x: 36
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            text: net.modelData.ssid
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                        }
                        Text {
                            text: (net.modelData.active ? qsTr("Connected, ") : "") + (net.modelData.secure ? qsTr("secured") : qsTr("open"))
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                    Row {
                        anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
                        spacing: 8
                        Button {
                            visible: net.modelData.known && !net.modelData.active
                            subtle: true
                            text: qsTr("Forget")
                            onClicked: Network.forget(net.modelData.ssid)
                        }
                        Button {
                            text: net.modelData.active ? qsTr("Disconnect") : qsTr("Connect")
                            onClicked: {
                                if (net.modelData.active) {
                                    Network.disconnectDevice(page.wifiDevice.device)
                                } else if (net.modelData.secure && (!net.modelData.known
                                    || Network.passwordRequiredSsid === net.modelData.ssid)) {
                                    passwordDialog.ssid = net.modelData.ssid
                                    password.text = ""
                                    passwordDialog.open()
                                    password.forceActiveFocus()
                                } else {
                                    Network.connectWifi(net.modelData.ssid, "")
                                }
                            }
                        }
                    }
                    MouseArea {
                        id: netMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }
                }
            }
        }
    }
    Text {
        visible: Network.connectResult !== "" && Network.connectResult !== "ok"
        width: parent.width
        text: Network.connectResult
        color: Theme.critical
        font.pixelSize: Theme.fontBody
        wrapMode: Text.Wrap
        topPadding: 4
        bottomPadding: 4
    }

    // --- Ethernet (and each connection's properties) ---
    Repeater {
        model: Network.devices
        delegate: Card {
            id: deviceCard
            required property var modelData
            icon: modelData.type === "wifi" ? "network-wireless" : "network-wired"
            title: (modelData.type === "wifi" ? qsTr("Wi-Fi") : qsTr("Ethernet")) + (Network.devices.filter(d => d.type === modelData.type).length > 1 ? " (" + modelData.device + ")" : "")
            description: page.stateText(modelData) + (modelData.connection !== "" && modelData.type === "ethernet" ? " · " + modelData.connection : "")
            visible: modelData.type === "ethernet" || (modelData.type === "wifi" && modelData.ip !== undefined)
            clickable: true
            onClicked: page.expanded = page.expanded === modelData.device ? "" : modelData.device
            trailing: Text {
                text: page.expanded === deviceCard.modelData.device ? "⌃" : "⌄"
                color: Theme.textSecondary
                font.pixelSize: 14
            }
            contentItem: Column {
                visible: page.expanded === deviceCard.modelData.device
                width: parent.width
                spacing: 6
                Repeater {
                    model: [
                        { label: qsTr("IPv4 address"), value: deviceCard.modelData.ip || "—" },
                        { label: qsTr("Gateway"), value: deviceCard.modelData.gateway || "—" },
                        { label: qsTr("DNS servers"), value: deviceCard.modelData.dns || "—" },
                        { label: qsTr("IPv6 address"), value: deviceCard.modelData.ipv6 || "—" },
                        { label: qsTr("Physical address (MAC)"), value: deviceCard.modelData.mac || "—" },
                        { label: qsTr("Interface"), value: deviceCard.modelData.device }
                    ]
                    delegate: Row {
                        required property var modelData
                        width: parent.width
                        Text {
                            width: 220
                            leftPadding: 36
                            text: modelData.label
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontBody
                        }
                        Text {
                            width: parent.width - 220
                            text: modelData.value
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.WrapAnywhere
                        }
                    }
                }
            }
        }
    }

    CardGroup {
        title: qsTr("Advanced network settings")
        LinkCard {
            visible: System.available("network")
            icon: "preferences-system-network"
            title: qsTr("Connections")
            description: qsTr("Static addresses, VPN, proxy and every connection option")
            onClicked: System.trigger("network")
        }
    }

    Dialog {
        id: passwordDialog
        parent: root.contentItem
        property string ssid
        title: qsTr("Connect to ") + ssid
        primaryText: qsTr("Next")
        primaryEnabled: password.text.length >= 8
        onAccepted: Network.connectWifi(ssid, password.text)
        Column {
            width: parent.width
            spacing: 8
            Text {
                text: qsTr("Enter the network security key")
                color: Theme.text
                font.pixelSize: Theme.fontBody
            }
            TextBox {
                id: password
                width: parent.width
                echoMode: TextInput.Password
                Keys.onReturnPressed: if (passwordDialog.primaryEnabled) { passwordDialog.close(); Network.connectWifi(passwordDialog.ssid, text) }
            }
        }
    }
}
