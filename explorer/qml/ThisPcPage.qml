// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// This PC, like Windows 11: the user's folders and the drives with a
// used-space bar (red when almost full).
Flickable {
    id: page
    property var tab
    contentHeight: column.height + 32
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    Component.onCompleted: Places.refreshDrives()
    Timer { interval: 5000; running: true; repeat: true; onTriggered: Places.refreshDrives() }

    Column {
        id: column
        x: 24
        y: 16
        width: page.width - 48
        spacing: 8

        Text {
            text: qsTr("Folders")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
            bottomPadding: 4
        }
        Flow {
            width: parent.width
            spacing: 4
            Repeater {
                model: Places.quickAccess
                delegate: Fill {
                    id: folder
                    required property var modelData
                    width: 220
                    height: 56
                    radius: Theme.radius
                    color: folderMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                    Image {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 36
                        height: 36
                        source: "image://fileicon/" + encodeURIComponent(folder.modelData.icon)
                        sourceSize: Qt.size(width, height)
                    }
                    Text {
                        x: 56
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 60
                        text: folder.modelData.name
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        id: folderMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onDoubleClicked: page.tab.navigate(folder.modelData.path)
                    }
                }
            }
        }

        Text {
            text: qsTr("Devices and drives")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
            topPadding: 20
            bottomPadding: 4
        }
        Flow {
            width: parent.width
            spacing: 4
            Repeater {
                model: Places.drives
                delegate: Fill {
                    id: drive
                    required property var modelData
                    readonly property bool mounted: modelData.mounted !== false
                    readonly property bool mounting: !mounted && page.tab.mountingVolume === modelData.volume
                    readonly property real used: mounted && modelData.total > 0 ? 1 - modelData.free / modelData.total : 0
                    function open() {
                        if (mounted) page.tab.navigate(modelData.path)
                        else page.tab.openVolume(modelData.volume)
                    }
                    width: 300
                    height: 72
                    opacity: mounted ? 1 : 0.85
                    radius: Theme.radius
                    color: driveMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                    Image {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 40
                        height: 40
                        source: "image://fileicon/" + encodeURIComponent(drive.modelData.icon)
                        sourceSize: Qt.size(width, height)
                    }
                    Column {
                        x: 60
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 72
                        spacing: 4
                        Text {
                            width: parent.width
                            text: drive.modelData.name + (drive.mounted && drive.modelData.path !== "/" ? "  (" + drive.modelData.path + ")" : "")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                        }
                        Rectangle {
                            visible: drive.mounted
                            width: parent.width
                            height: 12
                            color: Theme.light ? Qt.rgba(0, 0, 0, 0.08) : Qt.rgba(1, 1, 1, 0.12)
                            border.width: 1
                            border.color: Theme.divider
                            Rectangle {
                                x: 1
                                y: 1
                                height: parent.height - 2
                                width: (parent.width - 2) * drive.used
                                color: drive.used > 0.9 ? "#d13438" : Theme.accentFill
                            }
                        }
                        Text {
                            text: drive.mounting ? qsTr("Opening...")
                                : !drive.mounted ? Ops.formatSize(drive.modelData.total) + qsTr(", not opened yet")
                                : Ops.formatSize(drive.modelData.free) + qsTr(" free of ") + Ops.formatSize(drive.modelData.total)
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                    MouseArea {
                        id: driveMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onDoubleClicked: drive.open()
                        onClicked: mouse => {
                            if (mouse.button === Qt.RightButton) {
                                const p = mapToItem(null, mouse.x, mouse.y)
                                const d = drive.modelData
                                const entries = [{ text: qsTr("&Open"), icon: "document-open", action: () => drive.open() }]
                                if (drive.mounted) {
                                    entries.push(
                                        { text: qsTr("Open in new &tab"), icon: "tab-new", action: () => page.tab.openInNewTab(d.path) },
                                        { separator: true },
                                        { text: qsTr("Open in &Terminal"), icon: "utilities-terminal", action: () => System.openTerminal(d.path) })
                                    if (d.removable) entries.push({ text: qsTr("E&ject"), icon: "media-eject", action: () => Places.eject(d.device) })
                                    entries.push({ separator: true },
                                        { text: qsTr("P&roperties"), icon: "document-properties", action: () => Ops.showProperties([d.path]) })
                                }
                                page.tab.menus.open(entries, p.x, p.y)
                            }
                        }
                    }
                }
            }
        }
    }
}
