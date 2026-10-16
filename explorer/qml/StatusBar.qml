// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// The bottom line, like Windows: how many items, how many selected and how
// much they weigh; Details and Large icons on the right.
Item {
    id: bar
    property var model: null
    property string viewMode: "details"
    property string message // for pages without files (Home, This PC)
    signal viewRequested(string mode)
    height: 28

    function plural(n, one, many) {
        return n === 1 ? "1 " + one : n + " " + many
    }

    Text {
        anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
        color: Theme.text
        font.pixelSize: Theme.fontCaption
        text: {
            if (!bar.model) return bar.message
            let s = bar.plural(bar.model.count, qsTr("item"), qsTr("items"))
            if (bar.model.selectionCount > 0) {
                s += "      " + bar.plural(bar.model.selectionCount, qsTr("item selected"), qsTr("items selected"))
                if (bar.model.selectionSize > 0) s += "  " + Ops.formatSize(bar.model.selectionSize)
            }
            if (bar.model.loading) s += qsTr("      Loading...")
            return s
        }
    }
    Row {
        anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
        visible: bar.model !== null
        spacing: 2
        ToolButton {
            width: 28
            height: 24
            icon.name: "view-list-details"
            checked: bar.viewMode === "details"
            onClicked: bar.viewRequested("details")
        }
        ToolButton {
            width: 28
            height: 24
            icon.name: "view-list-icons"
            checked: bar.viewMode === "large"
            onClicked: bar.viewRequested("large")
        }
    }
}
