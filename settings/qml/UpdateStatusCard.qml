// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls
import Vela.Updates

// The top of System > Updates (docs/updates.md §9.1): the state in words,
// when it was last checked and the button: "Check for updates", or "Install
// now" when there is an update. Below: "Sign out to finish updating", the
// last check's failure beside the last completed check, the last install's
// failure while nothing happened since, and a refusal.
Card {
    id: card
    objectName: "status"

    readonly property bool hasUpdate: Updates.status === "update-available"
    // A completed check is shown; a failure then goes below it.
    readonly property bool checked: Updates.status !== "unknown" && Updates.status !== "error"
    readonly property bool checkFailed: Updates.checkError.code !== undefined
    // Not when a check came after it.
    readonly property bool installFailed: Updates.installError.code !== undefined
        && !(Updates.lastCheck.getTime() > Updates.installFinished.getTime())
    // What goes below (a child's `visible` is false while the column is
    // hidden, so the column can't follow them).
    readonly property bool showRelogin: Updates.available && Updates.needsRelogin
    readonly property bool showCheckFailure: Updates.available && checked && checkFailed && !Updates.busy
    readonly property bool showInstallFailure: Updates.available && installFailed && !Updates.busy
    readonly property bool showNextCheck: Updates.available && Updates.autoCheck && !Updates.busy
        && !isNaN(Updates.nextCheck.getTime())
    readonly property bool showNetworkWait: Updates.available && Updates.autoCheck && !Updates.busy
        && Updates.waitingForNetwork

    function phaseText(phase) {
        switch (phase) {
        case "resolving": return qsTr("getting ready")
        case "downloading": return qsTr("downloading")
        case "verifying": return qsTr("verifying")
        case "building": return qsTr("building")
        case "installing": return qsTr("installing")
        }
        return ""
    }
    function checkedText(date) {
        const time = date.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
        const day = new Date(date.getFullYear(), date.getMonth(), date.getDate())
        const today = new Date()
        today.setHours(0, 0, 0, 0)
        const days = Math.round((today.getTime() - day.getTime()) / 86400000)
        if (days === 0) {
            return qsTr("Last checked: today, %1").arg(time)
        }
        if (days === 1) {
            return qsTr("Last checked: yesterday, %1").arg(time)
        }
        return qsTr("Last checked: %1, %2").arg(date.toLocaleDateString(Qt.locale(), Locale.ShortFormat)).arg(time)
    }

    icon: !Updates.available || (!checked && checkFailed) ? "dialog-warning"
        : Updates.busy ? "system-software-update"
        : hasUpdate ? "update-high"
        : Updates.status === "up-to-date" ? "update-none"
        : "system-software-update"
    title: !Updates.available ? qsTr("The update service isn't running")
        : Updates.activity === "checking" ? qsTr("Checking for updates...")
        : Updates.activity === "starting" ? qsTr("Opening the terminal...")
        : Updates.activity === "installing" ? (phaseText(Updates.phase) !== ""
            ? qsTr("Updating in the terminal: %1").arg(phaseText(Updates.phase)) : qsTr("Updating in the terminal"))
        : hasUpdate ? qsTr("Vela %1 is available").arg(Updates.versionText)
        : Updates.status === "up-to-date" ? qsTr("You're up to date")
        : Updates.status === "downgrade-available" ? qsTr("You have a newer version than the latest release")
        : Updates.status === "not-installed" ? qsTr("Development build: not installed from a package")
        : checkFailed ? qsTr("Couldn't check for updates")
        : qsTr("Updates haven't been checked yet")
    description: !Updates.available ? qsTr("Vela can't check for updates or install them until it starts again.")
        : Updates.activity === "starting" || Updates.activity === "installing"
            ? qsTr("Follow the update in the terminal window. Your password is asked for there.")
        : !checked && checkFailed ? Updates.checkError.reason
        : !isNaN(Updates.lastCheck.getTime()) ? checkedText(Updates.lastCheck)
        : ""
    minimumHeight: 80

    trailing: Button {
        objectName: "action"
        visible: Updates.available
        text: card.hasUpdate ? qsTr("Install now") : qsTr("Check for updates")
        accent: card.hasUpdate
        usable: !Updates.busy
        onClicked: card.hasUpdate ? Updates.install({}) : Updates.check()
    }

    contentItem: Column {
        width: parent.width
        visible: card.showRelogin || card.showCheckFailure || card.showInstallFailure
            || card.showNextCheck || card.showNetworkWait || Updates.error !== ""
        spacing: 8

        Text {
            objectName: "nextCheck"
            visible: card.showNextCheck || card.showNetworkWait
            width: parent.width
            text: card.showNetworkWait
                ? qsTr("Next check: waiting for a fully connected, unmetered network")
                : card.showNextCheck ? qsTr("Next automatic check: %1").arg(
                    Updates.nextCheck.toLocaleString(Qt.locale(), Locale.ShortFormat)) : ""
            textFormat: Text.PlainText
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
        Text {
            objectName: "relogin"
            visible: card.showRelogin
            width: parent.width
            text: qsTr("Sign out to finish updating. Until then, the version you signed in with keeps running.")
            textFormat: Text.PlainText
            color: Theme.caution
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        Failure {
            objectName: "checkFailure"
            visible: card.showCheckFailure
            heading: qsTr("Couldn't check for updates: %1").arg(Updates.checkError.reason)
            message: Updates.checkError.message || ""
        }
        Failure {
            objectName: "installFailure"
            visible: card.showInstallFailure
            heading: qsTr("The last update didn't finish: %1").arg(Updates.installError.reason)
            message: Updates.installError.message || ""
        }
        Text {
            objectName: "error"
            visible: text !== ""
            width: parent.width
            text: Updates.error
            textFormat: Text.PlainText
            color: Theme.critical
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
    }

    // A failure: its reason, and vela-update's own message below it.
    component Failure: Column {
        property string heading
        property string message
        width: parent.width
        spacing: 2
        Text {
            width: parent.width
            text: parent.heading
            textFormat: Text.PlainText
            color: Theme.critical
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        Text {
            visible: text !== ""
            width: parent.width
            text: parent.message
            textFormat: Text.PlainText
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
    }
}
