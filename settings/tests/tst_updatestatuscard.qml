// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import Vela.Updates
import "../qml"

// The top of System > Updates (docs/updates.md §9.1): every state in words,
// the button for each, "Sign out to finish updating", failures with their
// reason and message, and the daemon missing.
Item {
    id: root
    width: 900
    height: 700

    UpdateStatusCard {
        id: card
        width: 700
    }

    TestCase {
        name: "UpdateStatusCard"
        when: windowShown

        function find(name) { return findChild(card, name) }
        function button() { return find("action") }
        // After the row has placed the button for its new text.
        function click(item) {
            waitForRendering(card)
            mouseClick(item)
        }

        function init() {
            Updates.reset()
        }

        function test_up_to_date() {
            compare(card.title, "You're up to date")
            compare(card.icon, "update-none")
            verify(card.description.startsWith("Last checked: today, "))
            compare(button().text, "Check for updates")
            verify(!button().accent)
            click(button())
            compare(Updates.calls, ["check"])
        }

        function test_last_checked_dates() {
            const yesterday = new Date()
            yesterday.setDate(yesterday.getDate() - 1)
            Updates.lastCheck = yesterday
            verify(card.description.startsWith("Last checked: yesterday, "))
            Updates.lastCheck = new Date(2026, 0, 15, 10, 24)
            verify(card.description.startsWith("Last checked: "))
            verify(card.description.indexOf("today") < 0 && card.description.indexOf("yesterday") < 0)
            Updates.lastCheck = new Date(NaN)
            Updates.status = "unknown"
            compare(card.title, "Updates haven't been checked yet")
            compare(card.description, "")
        }

        function test_update_available_installs() {
            Updates.status = "update-available"
            Updates.version = "0.1.1"
            Updates.commit = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            compare(card.title, "Vela 0.1.1 (bbbbbbb) is available")
            compare(button().text, "Install now")
            verify(button().accent)
            click(button())
            compare(Updates.calls, ["install"])
            // Beta's version is the release.
            Updates.channel = "beta"
            compare(card.title, "Vela 0.1.1 is available")
        }

        function test_busy() {
            Updates.activity = "checking"
            compare(card.title, "Checking for updates...")
            verify(!button().enabled)
            Updates.activity = "starting"
            compare(card.title, "Opening the terminal...")
            verify(card.description.indexOf("terminal") >= 0)
            Updates.activity = "installing"
            Updates.phase = "resolving"
            compare(card.title, "Updating in the terminal: getting ready")
            for (const [phase, words] of [["downloading", "downloading"], ["verifying", "verifying"],
                                          ["building", "building"], ["installing", "installing"]]) {
                Updates.phase = phase
                compare(card.title, "Updating in the terminal: " + words)
            }
            Updates.phase = ""
            compare(card.title, "Updating in the terminal")
            verify(!button().enabled)
            click(button())
            compare(Updates.calls, [])
        }

        function test_other_states() {
            Updates.status = "downgrade-available"
            compare(card.title, "You have a newer version than the latest release")
            Updates.status = "not-installed"
            compare(card.title, "Development build: not installed from a package")
            compare(button().text, "Check for updates")
        }

        // Never completed: the failure is the state. Completed once: it's
        // shown below the last completed check.
        function test_check_failures() {
            Updates.status = "error"
            Updates.lastCheck = new Date(NaN)
            Updates.checkError = Updates.failure("network", "offline")
            compare(card.title, "Couldn't check for updates")
            compare(card.description, "Reason: network")
            compare(card.icon, "dialog-warning")
            verify(!find("checkFailure").visible)

            Updates.status = "up-to-date"
            Updates.lastCheck = new Date()
            compare(card.title, "You're up to date")
            verify(find("checkFailure").visible)
            compare(find("checkFailure").heading, "Couldn't check for updates: Reason: network")
            compare(find("checkFailure").message, "offline")
            // Not while checking again.
            Updates.activity = "checking"
            verify(!find("checkFailure").visible)
        }

        // The last install's failure, until a check comes after it.
        function test_install_failure() {
            const now = new Date()
            Updates.lastCheck = new Date(now.getTime() - 60000)
            Updates.installResult = "failed"
            Updates.installError = Updates.failure("build-failed", "makepkg failed")
            Updates.installFinished = now
            verify(find("installFailure").visible)
            compare(find("installFailure").heading, "The last update didn't finish: Reason: build-failed")
            compare(find("installFailure").message, "makepkg failed")
            Updates.lastCheck = new Date(now.getTime() + 60000)
            verify(!find("installFailure").visible)
            // Never checked: still the last thing that happened.
            Updates.lastCheck = new Date(NaN)
            verify(find("installFailure").visible)
        }

        function test_sign_out_to_finish() {
            verify(!find("relogin").visible)
            Updates.needsRelogin = true
            verify(find("relogin").visible)
            verify(find("relogin").text.startsWith("Sign out to finish updating."))
        }

        function test_refusal() {
            verify(!find("error").visible)
            Updates.error = "An update or a check is already running."
            verify(find("error").visible)
            compare(find("error").text, "An update or a check is already running.")
        }

        function test_no_daemon() {
            Updates.available = false
            Updates.needsRelogin = true
            Updates.installError = Updates.failure("failed", "")
            compare(card.title, "The update service isn't running")
            compare(card.icon, "dialog-warning")
            verify(!button().visible)
            verify(!find("relogin").visible)
            verify(!find("installFailure").visible)
        }

        // Texts from the repository and the daemon are never markup.
        function test_plain_text() {
            Updates.status = "update-available"
            Updates.channel = "beta"
            Updates.version = "<b>1</b>"
            compare(card.title, "Vela <b>1</b> is available")
            compare(find("error").textFormat, Text.PlainText)
            compare(find("relogin").textFormat, Text.PlainText)
        }
    }
}
