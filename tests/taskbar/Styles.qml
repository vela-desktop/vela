// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton

import QtQuick

QtObject {
    property QtObject taskbar: QtObject {
        property int height: 48
        property bool floating: false
        property bool fit: false
        property int edge: 0
        property int splitGap: 0
        property int splitRadius: 5
        property int margin: 0
        property int sideMargin: 0
        property int radius: 0
        property bool flush: false
        property color stroke: "transparent"
        property int buttonWidth: 44
        property int buttonHeight: 40
        property int ownButtonRadius: 4
        property int buttonRadius: 4
        property int iconSize: 26
        property int spacing: 4
        function corner(gap, own) { return own }
    }
}
