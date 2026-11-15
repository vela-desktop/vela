// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton

import QtQuick

QtObject {
    property var preview: null
    property bool previewButtonHovered: false
    property var tooltip: null
    property string panelScreen: ""
    property bool notificationCenterOpen: false
    property bool quickSettingsOpen: false
    property bool isOpen: false
    property var entries: []
    function setTrayAnchors(screen, anchors) {}
    function placeTrayPanel(window, anchor, x, width) {}
    function open(items, x, y, options) { entries = items }
}
