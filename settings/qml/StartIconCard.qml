// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Dialogs
import QtCore
import Vela.Controls

// Start's "Start button icon": Vela's sail, the distribution's logo (only
// offered when the system has one, os-release's LOGO=) or a picture of your
// own, with the icon as the taskbar draws it beside the choice. Choosing
// "Picture" asks for one; cancelled, the choice stays what it was. "Browse"
// changes the picture.
Card {
    id: card
    // The choices offered, in order.
    readonly property var kinds: Prefs.distributionLogo !== "" ? ["vela", "distribution", "picture"] : ["vela", "picture"]
    readonly property string pictureName: {
        const path = Prefs.startIconSource
        return path.substring(path.lastIndexOf("/") + 1)
    }

    function chosenIndex() {
        return Math.max(0, kinds.indexOf(Prefs.startIcon))
    }

    icon: "start-here"
    title: qsTr("Start button icon")
    description: Prefs.startIcon === "distribution" ? qsTr("The logo of %1").arg(Prefs.distributionName)
        : Prefs.startIcon === "picture" ? qsTr("Your picture: %1").arg(pictureName)
        : qsTr("Vela's sail, in the accent color")
    trailing: [
        Item {
            objectName: "startIconPreview"
            anchors.verticalCenter: parent.verticalCenter
            width: 32
            height: 32
            StartIcon {
                anchors.centerIn: parent
                source: Prefs.startIconSource
            }
        },
        Button {
            objectName: "startIconBrowse"
            visible: Prefs.startIcon === "picture"
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Browse")
            onClicked: pictures.open()
        },
        Choice {
            id: choice
            objectName: "startIconChoice"
            anchors.verticalCenter: parent.verticalCenter
            model: card.kinds.map(kind => kind === "vela" ? "Vela"
                : kind === "distribution" ? (Prefs.distributionName || qsTr("Distribution"))
                : qsTr("Picture"))
            currentIndex: card.chosenIndex()
            onChosen: index => {
                const kind = card.kinds[index]
                if (kind === "picture") {
                    pictures.open()
                } else {
                    Prefs.setStartIcon(kind)
                }
                // Back on what Prefs says: the picture may be cancelled.
                currentIndex = Qt.binding(card.chosenIndex)
            }
        }
    ]

    FileDialog {
        id: pictures
        objectName: "startIconPictures"
        title: qsTr("Choose a picture for the Start button")
        currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        nameFilters: [qsTr("Images (*.svg *.png *.jpg *.jpeg *.webp *.bmp *.gif)")]
        onAccepted: Prefs.setStartIcon("picture", selectedFile.toString())
    }
}
