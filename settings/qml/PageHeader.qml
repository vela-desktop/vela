// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The page title with the path, like Windows 11: "System  ›  Display", where
// the earlier pieces are links.
Row {
    id: header
    property var crumbs: [] // [{name, title}]
    signal navigate(string name)
    spacing: 12

    Repeater {
        model: header.crumbs
        delegate: Row {
            required property var modelData
            required property int index
            readonly property bool current: index === header.crumbs.length - 1
            spacing: 12
            Text {
                id: crumb
                text: modelData.title
                color: parent.current ? Theme.text : crumbMouse.containsMouse ? Theme.textSecondary : Theme.textTertiary
                font.pixelSize: Theme.fontTitle
                // Expressive: M3's emphasized headline (Bold).
                font.weight: Theme.weightEmphasis
                MouseArea {
                    id: crumbMouse
                    anchors.fill: parent
                    enabled: !parent.parent.current
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: header.navigate(modelData.name)
                }
            }
            Text {
                visible: !parent.current
                anchors.verticalCenter: crumb.verticalCenter
                text: "›"
                color: Theme.textTertiary
                font.pixelSize: Theme.fontTitle - 4
            }
        }
    }
}
