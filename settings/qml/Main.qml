// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import Vela.Controls

// The Settings window, like Windows 11: account, search and sections on the
// left; the page on the right, with its path in the title.
Window {
    id: root
    width: 1100
    height: 760
    minimumWidth: 760
    minimumHeight: 500
    visible: true
    title: qsTr("Settings")
    color: Theme.background

    onActiveChanged: {
        Theme.windowActive = active
        if (active) {
            Prefs.reload() // the shell may have changed something (Do not disturb...)
        }
    }

    // --- the pages ---
    readonly property var pages: ({
        "home": { title: qsTr("Home"), parent: "", file: "HomePage.qml", icon: "go-home" },
        "system": { title: qsTr("System"), parent: "", file: "SystemPage.qml", icon: "computer" },
        "display": { title: qsTr("Display"), parent: "system", file: "DisplayPage.qml", icon: "video-display" },
        "sound": { title: qsTr("Sound"), parent: "system", file: "SoundPage.qml", icon: "audio-speakers" },
        "sound-device": { title: qsTr("Properties"), parent: "sound", file: "SoundDevicePage.qml", icon: "audio-card" },
        "volume-mixer": { title: qsTr("Volume mixer"), parent: "sound", file: "VolumeMixerPage.qml", icon: "view-media-equalizer" },
        "notifications": { title: qsTr("Notifications"), parent: "system", file: "NotificationsPage.qml", icon: "preferences-desktop-notification" },
        "power": { title: qsTr("Power"), parent: "system", file: "PowerPage.qml", icon: "preferences-system-power-management" },
        "about": { title: qsTr("About"), parent: "system", file: "AboutPage.qml", icon: "help-about" },
        "bluetooth": { title: qsTr("Bluetooth & devices"), parent: "", file: "BluetoothPage.qml", icon: "preferences-system-bluetooth" },
        "network": { title: qsTr("Network & internet"), parent: "", file: "NetworkPage.qml", icon: "preferences-system-network" },
        "personalization": { title: qsTr("Personalization"), parent: "", file: "PersonalizationPage.qml", icon: "preferences-desktop-theme" },
        "background": { title: qsTr("Background"), parent: "personalization", file: "BackgroundPage.qml", icon: "preferences-desktop-wallpaper" },
        "colors": { title: qsTr("Colors"), parent: "personalization", file: "ColorsPage.qml", icon: "preferences-desktop-color" },
        "taskbar": { title: qsTr("Taskbar"), parent: "personalization", file: "TaskbarPage.qml", icon: "preferences-system-windows" },
        "apps": { title: qsTr("Apps"), parent: "", file: "AppsPage.qml", icon: "preferences-desktop-default-applications" },
        "installed-apps": { title: qsTr("Installed apps"), parent: "apps", file: "InstalledAppsPage.qml", icon: "view-list-details" },
        "default-apps": { title: qsTr("Default apps"), parent: "apps", file: "DefaultAppsPage.qml", icon: "preferences-desktop-default-applications" },
        "default-app": { title: qsTr("Apps"), parent: "default-apps", file: "DefaultAppPage.qml", icon: "preferences-desktop-default-applications" },
        "time-language": { title: qsTr("Time & language"), parent: "", file: "TimeLanguagePage.qml", icon: "preferences-system-time" },
        "datetime": { title: qsTr("Date & time"), parent: "time-language", file: "DateTimePage.qml", icon: "preferences-system-time" },
        "keyboard": { title: qsTr("Keyboard"), parent: "time-language", file: "KeyboardPage.qml", icon: "input-keyboard" },
        "mouse": { title: qsTr("Mouse"), parent: "bluetooth", file: "MousePage.qml", icon: "input-mouse" },
        "touchpad": { title: qsTr("Touchpad"), parent: "bluetooth", file: "TouchpadPage.qml", icon: "input-touchpad" },
        "clipboard": { title: qsTr("Clipboard"), parent: "system", file: "ClipboardPage.qml", icon: "edit-paste" },
        "night-light": { title: qsTr("Night light"), parent: "display", file: "NightLightPage.qml", icon: "redshift-status-on" },
        "accessibility": { title: qsTr("Accessibility"), parent: "", file: "AccessibilityPage.qml", icon: "preferences-desktop-accessibility" }
    })
    readonly property var sections: ["home", "system", "bluetooth", "network", "personalization", "apps", "time-language", "accessibility"]
    // The names the shell uses (systemactions.cpp) for the menu entries.
    readonly property var aliases: ({
        "settings": "home", "personalize": "personalization", "taskbar-settings": "taskbar",
        "notification-settings": "notifications", "sound-settings": "sound", "devices": "bluetooth",
        "mobility": "power", "computer": "about", "installed-apps": "installed-apps",
        "bluetooth-settings": "bluetooth", "night-light-settings": "night-light", "accessibility-settings": "accessibility",
        "colors-settings": "colors"
    })

    // The output or input System > Sound > Properties shows: {kind, name}
    // (its node.name), and its device's name, which stays when the device
    // is turned off and its outputs and inputs are gone.
    property var soundDevice: ({ kind: "", name: "", device: "" })
    readonly property string soundDeviceTitle: {
        const list = soundDevice.kind === "output" ? Audio.outputs : soundDevice.kind === "input" ? Audio.inputs : []
        const endpoint = list.find(e => e.name === soundDevice.name)
        if (endpoint !== undefined) {
            return endpoint.description
        }
        const device = Audio.devices.find(d => d.name === soundDevice.device)
        return device !== undefined ? device.description : ""
    }
    function showSoundDevice(kind, name, device) {
        soundDevice = { kind: kind, name: name, device: device }
        navigate("sound-device")
    }

    property string current: "home"
    readonly property string section: {
        let name = current
        while (pages[name] && pages[name].parent !== "") {
            name = pages[name].parent
        }
        return name
    }
    readonly property var crumbs: {
        const list = []
        let name = current
        while (name !== "" && pages[name]) {
            // The app in Default apps has its name in the path.
            const title = name === "default-app" && DefaultApps.selectedName !== "" ? DefaultApps.selectedName
                : name === "sound-device" && soundDeviceTitle !== "" ? soundDeviceTitle
                : pages[name].title
            list.unshift({ name: name, title: title })
            name = pages[name].parent
        }
        // Home isn't a parent in the path, like on Windows.
        return list
    }

    function navigate(name, fromHistory) {
        name = aliases[name] || name
        if (!pages[name]) {
            name = "home"
        }
        if (!fromHistory && (name !== current || history.length === 0)) {
            const h = history.slice(0, historyIndex + 1)
            h.push(name)
            history = h
            historyIndex = h.length - 1
        }
        current = name
        search.text = ""
    }

    // The pages visited, for Back and Forward like Windows 11's Settings:
    // Alt+arrows, the keyboard's Back/Forward keys and the mouse's side
    // buttons (common/navigationbuttons.cpp), whatever has the focus.
    property var history: []
    property int historyIndex: -1
    function back() {
        if (historyIndex > 0) {
            historyIndex--
            navigate(history[historyIndex], true)
        }
    }
    function forward() {
        if (historyIndex < history.length - 1) {
            historyIndex++
            navigate(history[historyIndex], true)
        }
    }
    Shortcut { sequences: [StandardKey.Back]; onActivated: root.back() }
    Shortcut { sequences: [StandardKey.Forward]; onActivated: root.forward() }

    Component.onCompleted: navigate(Router.initialPage)

    Connections {
        target: Router
        function onPageRequested(page) {
            if (page !== "") {
                root.navigate(page)
            }
            if (root.visibility === Window.Minimized) {
                root.showNormal()
            }
            root.requestActivate()
        }
    }

    // --- search: "Find a setting" ---
    readonly property var searchIndex: [
        { page: "display", text: qsTr("Display"), keys: qsTr("resolution scale refresh rate hz orientation monitor multiple displays arrangement") },
        { page: "display", text: qsTr("Change the screen resolution"), keys: qsTr("resolution") },
        { page: "display", text: qsTr("Change the scale"), keys: qsTr("scale size text dpi zoom") },
        { page: "display", text: qsTr("Refresh rate"), keys: qsTr("hz refresh rate frequency") },
        { page: "sound", text: qsTr("Sound"), keys: qsTr("volume speakers headphones microphone output input sound") },
        { page: "sound", text: qsTr("Choose your output device"), keys: qsTr("output speakers headphones") },
        { page: "sound", text: qsTr("Microphone"), keys: qsTr("input recording level test") },
        { page: "volume-mixer", text: qsTr("Volume mixer"), keys: qsTr("app volume apps sound output input per app mixer reset") },
        { page: "sound", text: qsTr("Sample rate"), keys: qsTr("sample rate hz khz format bit depth quality resampling bit-perfect") },
        { page: "sound", text: qsTr("Restart audio"), keys: qsTr("restart audio sound stuck pipewire wireplumber reset") },
        { page: "notifications", text: qsTr("Notifications"), keys: qsTr("do not disturb alerts") },
        { page: "notifications", text: qsTr("Do not disturb"), keys: qsTr("silent notifications") },
        { page: "power", text: qsTr("Power"), keys: qsTr("screen off sleep idle battery energy saver lock") },
        { page: "power", text: qsTr("Turn off the screen after"), keys: qsTr("idle timeout") },
        { page: "power", text: qsTr("Power mode"), keys: qsTr("profile performance balanced") },
        { page: "about", text: qsTr("About"), keys: qsTr("specifications processor ram memory pc name rename version kernel graphics card") },
        { page: "about", text: qsTr("Rename this PC"), keys: qsTr("device name hostname") },
        { page: "bluetooth", text: qsTr("Bluetooth"), keys: qsTr("devices headphones mouse keyboard pair add") },
        { page: "bluetooth", text: qsTr("Add device"), keys: qsTr("pair bluetooth new") },
        { page: "network", text: qsTr("Network & internet"), keys: qsTr("wifi wi-fi ethernet cable ip dns connection") },
        { page: "network", text: qsTr("Wi-Fi"), keys: qsTr("wireless network password") },
        { page: "personalization", text: qsTr("Personalization"), keys: qsTr("theme appearance") },
        { page: "background", text: qsTr("Background"), keys: qsTr("picture desktop photo wallpaper") },
        { page: "colors", text: qsTr("Colors"), keys: qsTr("accent color theme dark light mode") },
        { page: "colors", text: qsTr("Choose your mode"), keys: qsTr("dark light theme apps mode") },
        { page: "taskbar", text: qsTr("Taskbar"), keys: qsTr("taskbar alignment left center end task") },
        { page: "apps", text: qsTr("Apps"), keys: qsTr("programs applications") },
        { page: "installed-apps", text: qsTr("Installed apps"), keys: qsTr("uninstall remove programs") },
        { page: "default-apps", text: qsTr("Default apps"), keys: qsTr("browser mail player open with default") },
        { page: "default-apps", text: qsTr("Choose the app for a file type"), keys: qsTr("pdf mp3 video music photos images extension file type link mailto archives zip documents") },
        { page: "default-apps", text: qsTr("Default browser"), keys: qsTr("web browser firefox chrome brave internet") },
        { page: "datetime", text: qsTr("Date & time"), keys: qsTr("clock time zone sync ntp") },
        { page: "time-language", text: qsTr("Language"), keys: qsTr("language english italian italiano inglese translation") },
            { page: "keyboard", text: qsTr("Keyboard"), keys: qsTr("layout language input typing key repeat") },
        { page: "mouse", text: qsTr("Mouse"), keys: qsTr("pointer speed primary button left-handed wheel scrolling lines acceleration precision") },
        { page: "touchpad", text: qsTr("Touchpad"), keys: qsTr("tap click gestures fingers scrolling laptop trackpad") },
        { page: "touchpad", text: qsTr("Touchpad gestures"), keys: qsTr("three fingers four fingers switch desktops apps") },
        { page: "clipboard", text: qsTr("Clipboard"), keys: qsTr("clipboard history win+v copy paste") },
        { page: "clipboard", text: qsTr("Snipping Tool"), keys: qsTr("screenshot capture print screen snip") },
        { page: "night-light", text: qsTr("Night light"), keys: qsTr("night warm colors blue evening schedule sunset") },
        { page: "display", text: qsTr("Tearing in games"), keys: qsTr("games tearing latency full screen vsync") },
        { page: "accessibility", text: qsTr("Accessibility"), keys: qsTr("low vision color blindness") },
        { page: "accessibility", text: qsTr("Magnifier"), keys: qsTr("zoom magnify magnifier") },
        { page: "accessibility", text: qsTr("Color filters"), keys: qsTr("grayscale color blindness deuteranopia protanopia tritanopia") },
        { page: "accessibility", text: qsTr("Sticky keys"), keys: qsTr("sticky keys shift ctrl alt keyboard") }
    ]
    readonly property var searchResults: {
        const q = search.text.trim().toLowerCase()
        if (q === "") {
            return []
        }
        return searchIndex.filter(e => e.text.toLowerCase().indexOf(q) >= 0 || e.keys.indexOf(q) >= 0).slice(0, 8)
    }

    // --- navigation on the left ---
    Item {
        id: nav
        x: 0
        y: 0
        width: 296
        height: parent.height

        // The account
        Row {
            id: account
            x: 16
            y: 16
            spacing: 14
            Rectangle {
                width: 64
                height: 64
                radius: 32
                color: Theme.accentFill
                Text {
                    anchors.centerIn: parent
                    text: About.userName.charAt(0).toUpperCase()
                    color: Theme.accentText
                    font.pixelSize: 26
                    font.weight: Font.DemiBold
                }
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                width: nav.width - 32 - 64 - 14
                Text {
                    width: parent.width
                    text: About.userName
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    text: qsTr("Local account")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                }
            }
        }

        TextBox {
            id: search
            x: 16
            anchors.top: account.bottom
            anchors.topMargin: 20
            width: nav.width - 32
            placeholderText: qsTr("Find a setting")
            rightPadding: 36
            Keys.onReturnPressed: {
                if (root.searchResults.length > 0) {
                    root.navigate(root.searchResults[0].page)
                }
            }
            Keys.onEscapePressed: text = ""
            Image {
                anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                width: 16
                height: 16
                source: Theme.icons + "edit-find"
                sourceSize: Qt.size(width, height)
                opacity: 0.8
            }
        }

        ListView {
            id: sectionList
            anchors { top: search.bottom; topMargin: 12; left: parent.left; right: parent.right; bottom: parent.bottom }
            leftMargin: 8
            rightMargin: 8
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
            model: root.sections
            spacing: 2
            delegate: Rectangle {
                id: entry
                required property string modelData
                readonly property bool selected: root.section === modelData
                width: sectionList.width - 16
                height: 36
                radius: Theme.radius
                color: entryMouse.pressed ? Theme.subtlePressed : selected || entryMouse.containsMouse ? Theme.subtleHover : "transparent"

                Rectangle {
                    visible: entry.selected
                    anchors.verticalCenter: parent.verticalCenter
                    width: 3
                    height: entryMouse.pressed ? 10 : 16
                    radius: 1.5
                    color: Theme.accentFill
                }
                Image {
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16
                    height: 16
                    source: "image://fileicon/" + encodeURIComponent(root.pages[entry.modelData].icon)
                    sourceSize: Qt.size(width, height)
                }
                Text {
                    x: 48
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.pages[entry.modelData].title
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                MouseArea {
                    id: entryMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.navigate(entry.modelData)
                }
            }
        }

        // Search results, below the box.
        Rectangle {
            visible: root.searchResults.length > 0 && search.activeFocus
            x: search.x
            anchors.top: search.bottom
            anchors.topMargin: 4
            width: search.width
            height: results.height + 8
            radius: Theme.radiusOverlay
            color: Theme.flyout
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 0.4)
            z: 10
            Column {
                id: results
                x: 4
                y: 4
                width: parent.width - 8
                Repeater {
                    model: root.searchResults
                    delegate: Rectangle {
                        id: result
                        required property var modelData
                        width: results.width
                        height: 40
                        radius: Theme.radius
                        color: resultMouse.containsMouse ? Theme.subtleHover : "transparent"
                        Image {
                            x: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16
                            height: 16
                            source: "image://fileicon/" + encodeURIComponent(root.pages[result.modelData.page].icon)
                            sourceSize: Qt.size(width, height)
                        }
                        Text {
                            x: 38
                            width: parent.width - 46
                            anchors.verticalCenter: parent.verticalCenter
                            text: result.modelData.text
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            id: resultMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: root.navigate(result.modelData.page)
                        }
                    }
                }
            }
        }
    }

    // --- the page ---
    Item {
        id: content
        anchors { left: nav.right; top: parent.top; bottom: parent.bottom; right: parent.right; leftMargin: 20 }

        PageHeader {
            id: header
            y: 20
            crumbs: root.crumbs
            onNavigate: name => root.navigate(name)
        }

        Loader {
            id: pageLoader
            anchors { top: header.bottom; topMargin: 20; left: parent.left; right: parent.right; bottom: parent.bottom }
            source: Qt.resolvedUrl(root.pages[root.current].file)
            onLoaded: {
                item.opacity = 0
                slide.y = 24
                enter.restart()
            }
            transform: Translate { id: slide }
            ParallelAnimation {
                id: enter
                NumberAnimation { target: pageLoader.item; property: "opacity"; to: 1; duration: Theme.normal }
                NumberAnimation { target: slide; property: "y"; to: 0; duration: Theme.long; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
            }
        }
    }
}
