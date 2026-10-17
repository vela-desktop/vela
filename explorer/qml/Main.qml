// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtCore
import Vela.Controls

// Explorer's window: tabs at the top (Ctrl+T, Ctrl+W, middle click on a
// folder), the open tab below. Menus, dialogs and copy progress live here too.
Window {
    id: root
    width: 1180
    height: 720
    minimumWidth: 640
    minimumHeight: 420
    visible: true
    color: Theme.background
    title: current ? current.title + qsTr(" - File Explorer") : qsTr("File Explorer")

    onActiveChanged: Theme.windowActive = active

    // --- preferences, remembered across launches ---
    Settings {
        id: settings
        property string viewModes: "{}" // folder -> view
        property bool showHidden: false
        property real navWidth: 240
        property string pane: "" // on the right: "", "preview" or "details"
        property real paneWidth: 320
    }
    property alias showHidden: settings.showHidden
    property alias navWidth: settings.navWidth
    property alias pane: settings.pane
    property alias paneWidth: settings.paneWidth
    // Alt+P and Alt+Shift+P: the preview or details pane, or nothing.
    function togglePane(mode) { pane = pane === mode ? "" : mode }
    property var viewModes: JSON.parse(settings.viewModes)
    function viewModeFor(location) {
        if (viewModes[location]) return viewModes[location]
        // Images and videos look better as large icons, like in Windows.
        if (/\/(Immagini|Pictures|Video|Videos)$/.test(location)) return "large"
        return "details"
    }
    function setViewMode(location, mode) {
        const copy = Object.assign({}, viewModes)
        copy[location] = mode
        viewModes = copy
        settings.viewModes = JSON.stringify(copy)
    }

    // --- tabs ---
    property var tabs: []
    property int currentIndex: 0
    readonly property var current: tabs[currentIndex] || null

    function newTab(location, select) {
        const tab = tabComponent.createObject(pages, { location: location })
        tab.navigate(location, select)
        tabs = tabs.concat([tab])
        currentIndex = tabs.length - 1
    }
    function closeTab(index) {
        const tab = tabs[index]
        const rest = tabs.slice()
        rest.splice(index, 1)
        tabs = rest
        tab.destroy()
        if (tabs.length === 0) {
            Qt.quit()
            return
        }
        currentIndex = Math.min(currentIndex, tabs.length - 1)
    }
    onCurrentIndexChanged: if (current) current.focusView()
    Component.onCompleted: newTab(StartLocation, StartSelection)

    Component {
        id: tabComponent
        TabPage {
            anchors.fill: parent
            menus: menuLayer
            win: root
            visible: root.current === this
        }
    }

    // Eject (from any tab): the error only once.
    Connections {
        target: Places
        function onEjected(error) { if (error !== "") root.showError(error) }
    }

    // Back and Forward: Alt+arrows, the keyboard's Back/Forward keys and the
    // mouse's side buttons (common/navigationbuttons.cpp), whatever has the
    // focus.
    Shortcut { sequences: [StandardKey.Back]; onActivated: if (root.current) root.current.back() }
    Shortcut { sequences: [StandardKey.Forward]; onActivated: if (root.current) root.current.forward() }
    Shortcut { sequence: "Ctrl+T"; onActivated: root.newTab("home:", "") }
    Shortcut { sequence: "Ctrl+W"; onActivated: root.closeTab(root.currentIndex) }
    Shortcut { sequence: "Ctrl+Tab"; onActivated: root.currentIndex = (root.currentIndex + 1) % root.tabs.length }
    Shortcut { sequence: "Ctrl+Shift+Tab"; onActivated: root.currentIndex = (root.currentIndex + root.tabs.length - 1) % root.tabs.length }
    Shortcut { sequence: "Ctrl+N"; onActivated: Ops.newWindow(root.current ? root.current.location : "") }
    Shortcut { sequences: ["Ctrl+L", "Alt+D", "F4"]; onActivated: root.focusChild("address") }
    Shortcut { sequences: ["Ctrl+F", "Ctrl+E", "F3"]; onActivated: root.focusChild("search") }
    // The open tab's address bar and search.
    function focusChild(what) {
        if (!current) return
        for (let i = 0; i < current.children.length; ++i) {
            const c = current.children[i]
            if (c.focusAddress) {
                what === "address" ? c.focusAddress() : c.focusSearch()
                return
            }
        }
    }

    // --- the tabs, at the top ---
    Item {
        id: strip
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 40

        Row {
            id: tabRow
            x: 8
            anchors.bottom: parent.bottom
            spacing: 2
            readonly property real tabWidth: Math.max(100, Math.min(240, (strip.width - 64) / Math.max(1, root.tabs.length)))

            Repeater {
                model: root.tabs
                delegate: Fill {
                    id: tabButton
                    required property var modelData
                    required property int index
                    readonly property bool active: root.currentIndex === index
                    width: tabRow.tabWidth
                    height: 34
                    radius: 0
                    topLeftRadius: Theme.radiusOverlay
                    topRightRadius: Theme.radiusOverlay
                    color: active ? Theme.layer : tabMouse.containsMouse ? Theme.subtleHover : Theme.subtleRest
                    Image {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        source: "image://fileicon/" + encodeURIComponent(tabButton.modelData.icon)
                        sourceSize: Qt.size(width, height)
                    }
                    Text {
                        x: 36
                        width: parent.width - 72
                        anchors.verticalCenter: parent.verticalCenter
                        text: tabButton.modelData.title
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption + 1
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                        onClicked: mouse => {
                            if (mouse.button === Qt.MiddleButton) {
                                root.closeTab(tabButton.index)
                            } else if (mouse.button === Qt.RightButton) {
                                const p = mapToItem(null, mouse.x, mouse.y)
                                menuLayer.open([
                                    { text: qsTr("&Duplicate tab"), icon: "tab-duplicate", action: () => root.newTab(tabButton.modelData.location, "") },
                                    { text: qsTr("Move to new &window"), icon: "window-new", action: () => { Ops.newWindow(tabButton.modelData.location); root.closeTab(tabButton.index) } },
                                    { separator: true },
                                    { text: qsTr("&Close tab"), icon: "tab-close", shortcut: "Ctrl+W", action: () => root.closeTab(tabButton.index) },
                                    { text: qsTr("Close &other tabs"), enabled: root.tabs.length > 1, action: () => {
                                        const keep = tabButton.modelData
                                        for (let i = root.tabs.length - 1; i >= 0; --i) if (root.tabs[i] !== keep) root.closeTab(i)
                                    } }
                                ], p.x, p.y)
                            } else {
                                root.currentIndex = tabButton.index
                            }
                        }
                    }
                    ToolButton {
                        anchors { right: parent.right; rightMargin: 6; verticalCenter: parent.verticalCenter }
                        width: 24
                        height: 24
                        icon.name: "window-close"
                        visible: tabMouse.containsMouse || tabButton.active || hovered
                        onClicked: root.closeTab(tabButton.index)
                    }
                    // Dragging files onto a tab opens it.
                    DropArea {
                        anchors.fill: parent
                        onEntered: root.currentIndex = tabButton.index
                    }
                }
            }
            ToolButton {
                anchors.verticalCenter: parent.verticalCenter
                width: 32
                height: 28
                icon.name: "list-add"
                onClicked: root.newTab("home:", "")
            }
        }
    }

    Item {
        id: pages
        anchors { left: parent.left; right: parent.right; top: strip.bottom; bottom: parent.bottom }
    }

    ProgressPanel {
        anchors { right: parent.right; bottom: parent.bottom; margins: 16; bottomMargin: 40 }
        z: 50
    }

    // --- dialogs ---
    function showError(message) {
        errorDialog.text = message
        errorDialog.open()
    }
    property var confirmAction: null
    function confirm(title, text, yes, action) {
        confirmDialog.title = title
        confirmDialog.text = text
        confirmDialog.buttons = [{ text: yes, accent: true, value: "yes" }, { text: qsTr("Cancel"), value: "cancel" }]
        confirmAction = action
        confirmDialog.open()
    }
    Connections {
        target: Ops
        function onFailed(message) { root.showError(message) }
        // The destination already has files with the same name: like Windows.
        function onConflict(count, firstName, directory) {
            conflictDialog.title = qsTr("Replace or skip files")
            conflictDialog.text = count === 1
                ? qsTr("The destination already has a file named \"") + firstName + "\"."
                : qsTr("The destination already has ") + count + qsTr(" files with the same name (\"") + firstName + qsTr("\" and others).")
            conflictDialog.open()
        }
    }
    Dialog {
        id: errorDialog
        title: qsTr("File Explorer")
        onChosen: if (root.current) root.current.focusView()
    }
    Dialog {
        id: confirmDialog
        onChosen: value => {
            if (value === "yes" && root.confirmAction) root.confirmAction()
            root.confirmAction = null
            if (root.current) root.current.focusView()
        }
    }
    Dialog {
        id: conflictDialog
        dialogWidth: 600
        buttons: [
            { text: qsTr("Replace"), accent: true, value: "replace" },
            { text: qsTr("Skip"), value: "skip" },
            { text: qsTr("Keep both"), value: "keep" },
            { text: qsTr("Cancel"), value: "cancel" }
        ]
        onChosen: value => Ops.resolveConflict(value)
    }

    MenuLayer {
        id: menuLayer
        anchors.fill: parent
        z: 100
        onVisibleChanged: if (!visible && root.current) root.current.focusView()
    }
}
