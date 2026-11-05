// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// Start's "Start button icon" (StartIconCard.qml): the distribution's logo
// is offered only when the system has one; "Picture" asks for a file and,
// cancelled, leaves the choice as it was; the preview draws what the
// taskbar draws.
Item {
    width: 900
    height: 300

    Column {
        width: parent.width
        StartIconCard {
            id: card
        }
    }

    TestCase {
        name: "StartIconCard"
        when: windowShown

        function choice() {
            return findChild(card, "startIconChoice")
        }
        function dialog() {
            return findChild(card, "startIconPictures")
        }
        function preview() {
            return findChild(card, "startIconPreview").children[0]
        }

        function init() {
            Prefs.reset()
        }
        function cleanup() {
            dialog().close()
        }

        function test_vela_by_default() {
            compare(choice().currentIndex, 0)
            compare(choice().currentText, "Vela")
            compare(preview().source, "")
            verify(!findChild(card, "startIconBrowse").visible)
        }

        function test_the_distribution_only_with_a_logo() {
            compare(choice().count, 3)
            compare(choice().model[1], "Test Linux")
            Prefs.distributionLogo = ""
            compare(choice().count, 2)
            compare(choice().model[1], "Picture")
        }

        function test_choosing_the_distribution() {
            choice().chosen(1)
            compare(Prefs.calls, [["setStartIcon", "distribution", ""]])
            compare(choice().currentIndex, 1)
            compare(preview().source, "test-logo")
            compare(card.description, "The logo of Test Linux")
            choice().chosen(0)
            compare(Prefs.startIcon, "vela")
            compare(preview().source, "")
        }

        function test_picture_cancelled_keeps_the_choice() {
            choice().chosen(1)
            choice().currentIndex = 2 // as a click on "Picture" leaves it
            choice().chosen(2)
            verify(dialog().visible, "no file asked for")
            compare(Prefs.calls.length, 1, "chose before a file was picked")
            dialog().reject()
            compare(Prefs.startIcon, "distribution")
            compare(choice().currentIndex, 1)
        }

        function test_picture_chosen() {
            // A picture in the tree: the dialog takes only files that exist.
            const picture = Qt.resolvedUrl("../../themes/screenshots/themes/vela.png").toString()
            choice().chosen(2)
            dialog().selectedFile = picture
            dialog().accepted()
            compare(Prefs.calls, [["setStartIcon", "picture", picture]])
            compare(choice().currentIndex, 2)
            compare(preview().source, picture.replace("file://", ""))
            compare(card.description, "Your picture: vela.png")
            // Browse changes it.
            const browse = findChild(card, "startIconBrowse")
            verify(browse.visible)
            mouseClick(browse)
            verify(dialog().visible)
        }
    }
}
