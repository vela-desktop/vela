// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls
import Vela.Updates

// System > Updates' options (docs/updates.md §9.1): automatic checks, the
// channel (asked first: the package is replaced, the version may go back,
// and the update runs in a terminal), Experimental's advanced options
// (another source, branch or commit) and the installed version.
CardGroup {
    id: options
    objectName: "options"
    // Where the channel's question is drawn: the window's content.
    property Item dialogParent: null
    property bool advancedOpen: false
    // The installed package's channel: what the last check was for.
    readonly property string channel: Updates.channel !== "" ? Updates.channel
        : Updates.installedPackage === "vela" ? "beta" : "experimental"
    readonly property int channelIndex: channel === "beta" ? 0 : 1
    readonly property var checkIntervals: [6, 12, 24, 48, 168]

    function channelName(name) {
        return name === "beta" ? qsTr("Beta") : qsTr("Experimental")
    }
    // A commit is 4 to 40 hexadecimal digits; anything else is a branch.
    function installRevision() {
        const chosen = {}
        const revision = revisionField.text.trim()
        if (/^[0-9a-fA-F]{4,40}$/.test(revision)) {
            chosen.commit = revision
        } else if (revision !== "") {
            chosen.branch = revision
        }
        if (sourceField.text.trim() !== "") {
            chosen.source = sourceField.text.trim()
        }
        Updates.install(chosen)
    }
    // The drop-down back on the installed channel, after a choice that
    // wasn't confirmed.
    function resetChannel() {
        channelChoice.currentIndex = channelIndex
        channelChoice.currentIndex = Qt.binding(() => options.channelIndex)
    }

    title: qsTr("More options")
    visible: Updates.available

    Card {
        icon: "view-refresh"
        title: qsTr("Check for updates automatically")
        description: qsTr("Not on metered connections")
        trailing: Switch {
            objectName: "autoCheck"
            checked: Updates.autoCheck
            Accessible.name: qsTr("Check for updates automatically")
            onToggled: Updates.setAutoCheck(checked)
        }
    }

    Card {
        icon: "appointment-new"
        title: qsTr("Automatic check frequency")
        description: qsTr("How often Vela checks for updates")
        trailing: Choice {
            objectName: "checkInterval"
            model: [qsTr("Every 6 hours"), qsTr("Every 12 hours"), qsTr("Every 24 hours"),
                    qsTr("Every 48 hours"), qsTr("Every 7 days")]
            currentIndex: options.checkIntervals.indexOf(Updates.checkInterval)
            usable: !Updates.busy
            Accessible.name: qsTr("Automatic check frequency")
            onChosen: index => Updates.setCheckInterval(options.checkIntervals[index])
        }
    }

    Card {
        icon: "vcs-branch"
        title: qsTr("Update channel")
        description: options.channel === "beta" ? qsTr("Tested, signed releases")
            : qsTr("The latest development version, as it's made")
        trailing: Choice {
            id: channelChoice
            objectName: "channel"
            model: [qsTr("Beta"), qsTr("Experimental")]
            currentIndex: options.channelIndex
            usable: !Updates.busy
            Accessible.name: qsTr("Update channel")
            onChosen: index => {
                const chosen = index === 0 ? "beta" : "experimental"
                if (chosen !== options.channel) {
                    channelDialog.start(chosen)
                }
            }
        }
    }

    Card {
        id: advanced
        objectName: "advanced"
        visible: options.channel === "experimental"
        icon: "configure"
        title: qsTr("Advanced options")
        description: qsTr("Install from another repository, branch or commit")
        clickable: true
        onClicked: options.advancedOpen = !options.advancedOpen
        trailing: Text {
            text: options.advancedOpen ? "⌃" : "⌄"
            color: Theme.textSecondary
            font.pixelSize: 14
        }
        contentItem: Column {
            visible: options.advancedOpen
            width: parent.width
            spacing: 6
            Text {
                text: qsTr("Source: a repository URL or a folder")
                color: Theme.text
                font.pixelSize: Theme.fontBody
            }
            TextBox {
                id: sourceField
                objectName: "source"
                width: Math.min(parent.width, 480)
                placeholderText: Updates.source
                Accessible.name: qsTr("Source")
            }
            Text {
                topPadding: 6
                text: qsTr("Branch or commit")
                color: Theme.text
                font.pixelSize: Theme.fontBody
            }
            TextBox {
                id: revisionField
                objectName: "revision"
                width: Math.min(parent.width, 480)
                placeholderText: qsTr("The default branch")
                Accessible.name: qsTr("Branch or commit")
                Keys.onReturnPressed: if (installButton.enabled) options.installRevision()
                Keys.onEnterPressed: if (installButton.enabled) options.installRevision()
            }
            Item { width: 1; height: 4 }
            Button {
                id: installButton
                objectName: "installRevision"
                text: qsTr("Install this revision")
                usable: !Updates.busy && (sourceField.text.trim() !== "" || revisionField.text.trim() !== "")
                onClicked: options.installRevision()
            }
        }
    }

    Card {
        objectName: "installed"
        visible: Updates.installedVersion !== "" || Updates.status === "not-installed"
        icon: "help-about"
        title: qsTr("Installed version")
        description: Updates.installedVersion !== ""
            ? qsTr("%1, %2 channel").arg(Updates.installedVersion).arg(options.channelName(options.channel))
            : qsTr("Not installed from a package")
    }

    Dialog {
        id: channelDialog
        objectName: "channelDialog"
        parent: options.dialogParent
        property string channel
        function start(name) {
            channel = name
            open()
        }
        title: channel === "beta" ? qsTr("Switch to Beta?") : qsTr("Switch to Experimental?")
        buttons: [{ text: qsTr("Switch"), accent: true, value: "switch" }, { text: qsTr("Cancel"), value: "cancel" }]
        onChosen: value => {
            if (value === "switch") {
                Updates.install({ channel: channel })
            }
        }
        onVisibleChanged: if (!visible) options.resetChannel()
        Text {
            width: parent.width
            text: (channelDialog.channel === "beta"
                ? qsTr("Beta installs tested, signed releases. The vela-git package is replaced by vela, and Vela may go back to an older version than the one you have.")
                : qsTr("Experimental installs the latest development version, downloaded or built on this PC. It changes often and may be less stable. The vela package is replaced by vela-git."))
                + "\n\n" + qsTr("The update runs in a terminal, where your password is asked for.")
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
    }
}
