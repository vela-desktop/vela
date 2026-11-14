// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

Page {
    CardGroup {
        LinkCard {
            icon: "video-display"
            title: qsTr("Display")
            description: qsTr("Monitors, brightness, resolution, scale")
            onClicked: root.navigate("display")
        }
        LinkCard {
            icon: "audio-speakers"
            title: qsTr("Sound")
            description: qsTr("Volume levels, output, input, sound devices")
            onClicked: root.navigate("sound")
        }
        LinkCard {
            icon: "preferences-desktop-notification"
            title: qsTr("Notifications")
            description: qsTr("Alerts from apps and the system, do not disturb, their number by the clock")
            onClicked: root.navigate("notifications")
        }
        LinkCard {
            icon: "preferences-system-power-management"
            title: qsTr("Power")
            description: Status.batteryPresent ? qsTr("Sleep, battery usage, energy saver") : qsTr("Screen and sleep, power mode")
            onClicked: root.navigate("power")
        }
        LinkCard {
            icon: "edit-paste"
            title: qsTr("Clipboard")
            description: qsTr("Clipboard history (Win+V), clear")
            onClicked: root.navigate("clipboard")
        }
        Card {
            objectName: "temporaryCredentials"
            icon: "dialog-password"
            title: qsTr("Password manager (temporary)")
            description: !Prefs.secretBackendConfigured && !Prefs.secretBackendInstalled
                ? qsTr("Choose your password manager. The temporary KWallet fallback is not installed on this computer. No passwords are transferred; sign out and back in after choosing.")
                : !Prefs.secretBackendConfigured
                ? qsTr("Choose your existing password manager. KWallet is used only as a fallback until you choose. No passwords are transferred; sign out and back in to apply.")
                : !Prefs.secretBackendInstalled
                ? qsTr("The selected password manager is not installed. Install it or choose the other provider, then sign out and back in.")
                : Prefs.secretBackend !== Prefs.activeSecretBackend
                ? qsTr("This password manager will be used after signing out and back in. The running session still uses the previous choice.")
                : Prefs.credentialStatus === "conflict"
                ? qsTr("Another password manager owns Secret Service. Your choice is not active; check the Vela session log. No passwords were migrated.")
                : Prefs.credentialStatus === "missing" || Prefs.credentialStatus === "unverified" || Prefs.credentialStatus === "failed"
                ? qsTr("The selected password manager could not be verified or started. Check the session log. Changing the choice requires a new login.")
                : qsTr("Use this existing password store for apps. No passwords are transferred. Changes take effect after signing out and back in.")
            trailing: Choice {
                objectName: "secretBackend"
                model: [qsTr("KWallet (KDE)"), qsTr("GNOME Keyring")]
                currentIndex: Prefs.secretBackendConfigured ? (Prefs.secretBackend === "gnome" ? 1 : 0) : -1
                Accessible.name: qsTr("Select password manager")
                onChosen: index => Prefs.secretBackend = ["kwallet", "gnome"][index]
            }
        }
        LinkCard {
            icon: "system-software-update"
            title: qsTr("Updates")
            description: qsTr("Check for updates, what's new, update channel")
            onClicked: root.navigate("updates")
        }
        LinkCard {
            icon: "help-about"
            title: qsTr("About")
            description: qsTr("Device specifications, rename your PC, operating system specifications")
            onClicked: root.navigate("about")
        }
    }
}
