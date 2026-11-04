// SPDX-FileCopyrightText: 2026 lowell
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Layouts
import QtCore
import Vela.Controls

// Windows 7's Start menu (startmenu-layout=1, Styles.startMenu.seven), inside
// StartMenu's panel: the programs on white at the left with the search at its
// bottom, the user, the places and Shut down on the glass at the right. The
// frames are AeroThemePlasma's SevenStart (shell/data/seven, GPL-3.0). The
// search field stays StartMenu's: it goes where `searchArea` says.
Item {
    id: seven

    // StartMenu.qml: its pinned apps, close(), openAppMenu()...
    required property var menu
    // The search's text: the list shows the matching apps instead.
    property bool searching: false
    // Where the search field sits, in this item's coordinates: centered in
    // the light strip at the bottom of the white column. From the geometry,
    // not mapToItem(), so it follows the layout.
    readonly property rect searchArea: Qt.rect(columns.x + leftColumn.x + 10,
        columns.y + leftColumn.y + leftColumn.height - 2 - (searchStrip.height + 30) / 2,
        leftColumn.width - 20, 30)

    property bool allPrograms: false // "All Programs": every app, not only the pinned ones
    property bool powerOpen: false
    function reset() {
        allPrograms = false
        powerOpen = false
    }

    // The pinned apps and then the first others, up to twelve; every app with
    // "All Programs"; the matching ones while searching (Apps.query).
    readonly property var programs: {
        if (!visible) {
            return [] // not Windows 7's layout: nothing to list
        }
        Apps.catalogRevision
        void Apps.query
        void Apps.count
        const out = []
        const seen = {}
        if (!searching && !allPrograms) {
            for (const p of menu.pinned) {
                seen[p.id] = true
                out.push(p)
            }
        }
        for (let i = 0; i < Apps.count && (searching || allPrograms || out.length < 12); ++i) {
            const id = Apps.idAt(i)
            if (seen[id]) {
                continue
            }
            const e = Apps.entry(id)
            out.push({ id: id, name: e.name, iconName: e.iconName })
        }
        return out
    }

    // The glass: a white line just inside the panel's dark one, and Windows 7's
    // darkening, light at the edges and dark in the middle.
    Rectangle {
        anchors { fill: parent; margins: 1 }
        radius: parent.parent ? parent.parent.radius - 1 : 0
        color: "transparent"
        border.width: 1
        border.color: "#80FFFFFF"
    }
    Rectangle {
        anchors { fill: parent; margins: 2 }
        readonly property real corner: parent.parent ? parent.parent.radius - 2 : 0
        topLeftRadius: corner
        topRightRadius: corner
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#26000000" }
            GradientStop { position: 0.11; color: "#53000000" }
            GradientStop { position: 0.2775; color: "#82000000" }
            GradientStop { position: 0.445; color: "#99000000" }
            GradientStop { position: 0.51; color: "#99000000" }
            GradientStop { position: 0.84; color: "#4F000000" }
            GradientStop { position: 1.0; color: "#00000000" }
        }
    }

    RowLayout {
        id: columns
        anchors { fill: parent; margins: 6 }
        spacing: 4

        // The left column: white, the search in a light strip at its bottom.
        BorderImage {
            id: leftColumn
            Layout.preferredWidth: 312
            Layout.fillHeight: true
            source: "qrc:/seven/panel.png"
            border { left: 6; top: 6; right: 6; bottom: 6 }

            Rectangle {
                id: searchStrip
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 2 }
                height: 52
                radius: 3
                color: "#F1F5FB"
                Rectangle { // its top edge
                    anchors { left: parent.left; right: parent.right; top: parent.top }
                    height: 4
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#CCD9EA" }
                        GradientStop { position: 1; color: "#F0F4FA" }
                    }
                }
            }

            ColumnLayout {
                anchors { fill: parent; margins: 4; bottomMargin: searchStrip.height + 4 }
                spacing: 4

                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: seven.programs
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: Item {
                        id: row
                        required property var modelData
                        width: list.width
                        height: 41
                        BorderImage {
                            anchors.fill: parent
                            visible: rowMouse.containsMouse
                            source: "qrc:/seven/item-hover.png"
                            border { left: 2; top: 2; right: 2; bottom: 2 }
                        }
                        Image {
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 32
                            height: 32
                            source: Theme.icons + encodeURIComponent(row.modelData.iconName ?? "")
                            sourceSize: Qt.size(width, height)
                            smooth: true
                        }
                        Text {
                            anchors { left: parent.left; leftMargin: 54; right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                            text: row.modelData.name
                            color: "#3C3C3C"
                            elide: Text.ElideRight
                            font.pixelSize: 15
                        }
                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: mouse => {
                                if (mouse.button === Qt.RightButton) {
                                    seven.menu.openAppMenu(row, mouse.x, mouse.y, row.modelData.id, false)
                                } else if (Apps.launchId(row.modelData.id)) {
                                    seven.menu.close()
                                }
                            }
                        }
                    }
                }

                Rectangle { // the line above "All Programs"
                    visible: !seven.searching
                    Layout.fillWidth: true
                    Layout.leftMargin: 6
                    Layout.rightMargin: 6
                    Layout.preferredHeight: 1
                    color: "#D6E5F5"
                }

                Item { // "All Programs" / "Back"
                    visible: !seven.searching
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 34 : 0
                    BorderImage {
                        anchors.fill: parent
                        visible: allMouse.containsMouse
                        source: "qrc:/seven/item-hover.png"
                        border { left: 2; top: 2; right: 2; bottom: 2 }
                    }
                    Text {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: seven.allPrograms ? "◂  " + qsTr("Back") : "▸  " + qsTr("All Programs")
                        color: "#3C3C3C"
                        font.pixelSize: 15
                    }
                    MouseArea {
                        id: allMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: seven.allPrograms = !seven.allPrograms
                    }
                }
            }
        }

        // The right column, straight on the glass: the user, the places, Shut down.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                id: rightColumn
                anchors { fill: parent; margins: 6 }
                spacing: 0

                // The user's picture in Windows 7's frame; without one, a portrait drawn.
                Item {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: -2
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: 72
                    Rectangle {
                        anchors { fill: parent; margins: 8 }
                        color: "#8AA9CC"
                        clip: true
                        Image {
                            id: picture
                            anchors { fill: parent; margins: 1 }
                            source: Shell.userPicture
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                        Item { // a head and shoulders
                            anchors.fill: parent
                            visible: picture.status !== Image.Ready
                            Rectangle { x: 15; y: 8; width: 18; height: 18; radius: 9; color: "#E6F0FA" }
                            Rectangle { x: 7; y: 28; width: 34; height: 24; radius: 12; color: "#E6F0FA" }
                        }
                    }
                    BorderImage {
                        anchors.fill: parent
                        source: "qrc:/seven/avatar-frame.png"
                        border { left: 11; top: 11; right: 11; bottom: 11 }
                    }
                }
                Text {
                    Layout.topMargin: 6
                    Layout.leftMargin: 6
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: Shell.userName
                    color: "white"
                    font.pixelSize: 15
                }
                ColumnLayout {
                    Layout.topMargin: 8
                    Layout.fillWidth: true
                    spacing: 0
                    Repeater {
                        // folder: a place of the home; action: a System action (Settings' pages).
                        model: [
                            { text: qsTr("Documents"), folder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation) },
                            { text: qsTr("Pictures"), folder: StandardPaths.writableLocation(StandardPaths.PicturesLocation) },
                            { text: qsTr("Music"), folder: StandardPaths.writableLocation(StandardPaths.MusicLocation) },
                            { rule: true },
                            { text: qsTr("Games"), folder: StandardPaths.writableLocation(StandardPaths.HomeLocation) + "/Games" },
                            { text: qsTr("Computer"), folder: StandardPaths.writableLocation(StandardPaths.HomeLocation) },
                            { rule: true },
                            { text: qsTr("Control Panel"), action: "settings" },
                            { text: qsTr("Devices and Printers"), action: "bluetooth-settings" },
                            { text: qsTr("Default Programs"), action: "default-apps-settings" },
                            { text: qsTr("Help and Support"), action: "system" }
                        ]
                        delegate: Item {
                            id: place
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.preferredHeight: modelData.rule ? 14 : 32
                            Rectangle {
                                visible: place.modelData.rule === true
                                anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 6; rightMargin: 6 }
                                height: 1
                                color: "#4DFFFFFF"
                            }
                            BorderImage {
                                anchors.fill: parent
                                visible: !place.modelData.rule && placeMouse.containsMouse
                                source: "qrc:/seven/place-hover.png"
                                border { left: 6; top: 5; right: 5; bottom: 5 }
                            }
                            Text {
                                anchors { left: parent.left; right: parent.right; leftMargin: 8; rightMargin: 4; verticalCenter: parent.verticalCenter }
                                visible: !place.modelData.rule
                                elide: Text.ElideRight
                                text: place.modelData.text ?? ""
                                color: "white"
                                font.pixelSize: 15
                            }
                            MouseArea {
                                id: placeMouse
                                anchors.fill: parent
                                enabled: !place.modelData.rule
                                hoverEnabled: true
                                onClicked: {
                                    if (place.modelData.action !== undefined) {
                                        System.trigger(place.modelData.action)
                                    } else {
                                        System.openUrl(place.modelData.folder)
                                    }
                                    seven.menu.close()
                                }
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true } // the places above, Shut down below

                // Shut down, with the arrow for the rest (AeroThemePlasma's
                // startmenu-buttons.svg: normal, hover, pressed).
                Item {
                    id: shutDown
                    readonly property bool hot: shutMouse.containsMouse || arrowMouse.containsMouse || seven.powerOpen
                    Layout.preferredWidth: Math.min(rightColumn.width - 4, 138)
                    Layout.preferredHeight: 30
                    Layout.bottomMargin: 7 // level with the search field
                    BorderImage {
                        anchors { left: parent.left; top: parent.top; bottom: parent.bottom; right: arrow.left }
                        source: shutMouse.pressed ? "qrc:/seven/shutdown-pressed.png" : shutDown.hot ? "qrc:/seven/shutdown-hover.png" : "qrc:/seven/shutdown.png"
                        border { left: 3; top: 3; right: 3; bottom: 3 }
                    }
                    Text {
                        anchors { horizontalCenter: parent.horizontalCenter; horizontalCenterOffset: -arrow.width / 2; verticalCenter: parent.verticalCenter; verticalCenterOffset: -1 }
                        text: qsTr("Shut down")
                        color: "white"
                        font.pixelSize: 15
                    }
                    MouseArea {
                        id: shutMouse
                        anchors { left: parent.left; top: parent.top; bottom: parent.bottom; right: arrow.left }
                        hoverEnabled: true
                        onClicked: Shell.powerOff()
                    }
                    BorderImage {
                        id: arrow
                        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
                        width: 30
                        source: arrowMouse.pressed ? "qrc:/seven/arrow-pressed.png" : shutDown.hot ? "qrc:/seven/arrow-hover.png" : "qrc:/seven/arrow.png"
                        border { left: 3; top: 3; right: 3; bottom: 3 }
                        Text {
                            anchors.centerIn: parent
                            text: "▸"
                            color: "white"
                            font.pixelSize: 12
                        }
                        MouseArea {
                            id: arrowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: seven.powerOpen = !seven.powerOpen
                        }
                    }

                    // The arrow's menu, above the button: light, like Windows 7's.
                    Rectangle {
                        visible: seven.powerOpen
                        anchors { right: parent.right; bottom: parent.top; bottomMargin: 4 }
                        width: 220
                        height: powerEntries.implicitHeight + 8
                        radius: 3
                        color: "#F2F2F2"
                        border.width: 1
                        border.color: "#979797"
                        z: 10
                        Column {
                            id: powerEntries
                            x: 4
                            y: 4
                            width: parent.width - 8
                            Repeater {
                                model: [
                                    { text: qsTr("Sleep"), icon: "system-suspend", shown: seven.menu.canSuspend, run: () => { seven.menu.close(); Shell.suspend() } },
                                    { text: qsTr("Restart"), icon: "system-reboot", shown: true, run: () => Shell.reboot() },
                                    { text: qsTr("Sign out"), icon: "system-log-out", shown: true, run: () => Shell.logout() }
                                ]
                                delegate: Fill {
                                    id: powerEntry
                                    required property var modelData
                                    visible: modelData.shown
                                    width: parent.width
                                    height: visible ? 36 : 0
                                    radius: 3
                                    color: entryMouse.containsMouse ? "#DCE8F7" : "transparent"
                                    Image {
                                        x: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 16
                                        height: 16
                                        source: "image://icon/k/" + powerEntry.modelData.icon // dark glyphs: the menu is light
                                        sourceSize: Qt.size(width, height)
                                    }
                                    Text {
                                        x: 36
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: powerEntry.modelData.text
                                        color: "#1E1E1E"
                                        font.pixelSize: Theme.fontBody
                                    }
                                    MouseArea {
                                        id: entryMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: powerEntry.modelData.run()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
