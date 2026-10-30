// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Updates
import "../qml"

// System > Updates' options (docs/updates.md §9.1): the automatic checks
// switch, the channel asked first and back when not confirmed,
// Experimental's advanced options, and the installed version.
Item {
    id: root
    width: 900
    height: 900

    UpdateOptions {
        id: options
        width: 700
        dialogParent: root
    }

    TestCase {
        name: "UpdateOptions"
        when: windowShown

        function find(name) { return findChild(root, name) }
        function type(text) {
            for (const c of text)
                keyClick(c)
        }
        // As from the keyboard: the closed drop-down moves to the
        // neighbour and reports the choice.
        function choose(index) {
            const choice = find("channel")
            choice.forceActiveFocus()
            if (index === choice.currentIndex) {
                choice.activated(index)
                return
            }
            keyClick(index < choice.currentIndex ? Qt.Key_Up : Qt.Key_Down)
            compare(choice.currentIndex, index)
        }

        function init() {
            Updates.reset()
            options.advancedOpen = false
        }
        function cleanup() {
            const dialog = find("channelDialog")
            if (dialog.visible) {
                dialog.close()
                tryVerify(() => !dialog.visible)
            }
            find("source").text = ""
            find("revision").text = ""
        }

        function test_automatic_checks() {
            const toggle = find("autoCheck")
            verify(toggle.checked)
            mouseClick(toggle)
            compare(Updates.calls, ["setAutoCheck false"])
            verify(!toggle.checked)
            Updates.autoCheck = true
            verify(toggle.checked)
        }

        function test_channel_change_is_asked_first() {
            compare(find("channel").currentIndex, 1)
            choose(0)
            const dialog = find("channelDialog")
            tryVerify(() => dialog.visible)
            compare(dialog.title, "Switch to Beta?")
            compare(Updates.calls, [])
            dialog.close("switch")
            compare(Updates.calls, ["install channel=beta"])
            // Back on the installed one until the update changes it.
            tryVerify(() => !dialog.visible)
            compare(find("channel").currentIndex, 1)
        }

        function test_channel_change_cancelled() {
            choose(0)
            const dialog = find("channelDialog")
            tryVerify(() => dialog.visible)
            keyClick(Qt.Key_Escape)
            tryVerify(() => !dialog.visible)
            compare(Updates.calls, [])
            compare(find("channel").currentIndex, 1)
            // Still follows the installed channel afterwards.
            Updates.channel = "beta"
            compare(find("channel").currentIndex, 0)
            choose(1)
            tryVerify(() => dialog.visible)
            compare(dialog.title, "Switch to Experimental?")
        }

        function test_same_channel_asks_nothing() {
            choose(1)
            wait(50)
            verify(!find("channelDialog").visible)
            compare(Updates.calls, [])
        }

        function test_channel_from_the_package() {
            Updates.channel = ""
            Updates.installedPackage = "vela"
            compare(find("channel").currentIndex, 0)
            Updates.installedPackage = "vela-git"
            compare(find("channel").currentIndex, 1)
        }

        function test_not_while_busy() {
            Updates.activity = "installing"
            verify(!find("channel").enabled)
            options.advancedOpen = true
            find("revision").text = "main"
            verify(!find("installRevision").enabled)
        }

        // Experimental only; a commit is hexadecimal, anything else a branch.
        function test_advanced_options() {
            const advanced = find("advanced")
            verify(advanced.visible)
            verify(!find("revision").visible)
            mouseClick(advanced, 40, 30)
            verify(options.advancedOpen)
            tryVerify(() => find("revision").visible)
            verify(!find("installRevision").enabled)
            compare(find("source").placeholderText, "https://github.com/vela-desktop/vela.git")

            find("revision").forceActiveFocus()
            type("fix/nvidia")
            mouseClick(find("installRevision"))
            compare(Updates.calls, ["install branch=fix/nvidia"])

            find("revision").text = "abc1234"
            find("source").text = " /home/me/vela "
            mouseClick(find("installRevision"))
            compare(Updates.calls[1], "install commit=abc1234 source=/home/me/vela")

            // Enter installs too.
            find("revision").text = "main"
            find("revision").forceActiveFocus()
            keyClick(Qt.Key_Return)
            compare(Updates.calls[2], "install branch=main source=/home/me/vela")

            Updates.channel = "beta"
            verify(!advanced.visible)
        }

        function test_installed_version() {
            const installed = find("installed")
            verify(installed.visible)
            compare(installed.description, "0.1.0.r10.gaaaaaaa-1, Experimental channel")
            Updates.installedVersion = ""
            Updates.status = "not-installed"
            compare(installed.description, "Not installed from a package")
            Updates.status = "unknown"
            verify(!installed.visible)
        }

        function test_no_daemon() {
            Updates.available = false
            verify(!options.visible)
        }
    }
}
