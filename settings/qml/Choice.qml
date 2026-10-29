// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic as C
import Vela.Controls

// The Windows 11 drop-down (ComboBox). `model` is a list of strings or of
// objects with `text`; `currentIndex` the chosen one.
C.ComboBox {
    id: combo
    property bool usable: true
    signal chosen(int index)

    enabled: usable
    implicitWidth: Math.max(160, Math.min(320, contentWidth + 56))
    implicitHeight: 32
    textRole: model && model.length > 0 && typeof model[0] === "object" ? "text" : ""
    font.pixelSize: Theme.fontBody
    onActivated: index => combo.chosen(index)

    property real contentWidth: {
        let widest = 0
        const items = model || []
        for (let i = 0; i < items.length; ++i) {
            widest = Math.max(widest, metrics.advanceWidth(typeof items[i] === "object" ? items[i].text : items[i]))
        }
        return widest
    }
    FontMetrics { id: metrics; font.pixelSize: Theme.fontBody }

    background: Fill {
        color: combo.pressed ? Theme.controlPressed : combo.hovered ? Theme.controlHover : Theme.control
        border.width: 1
        border.color: Theme.controlStroke
        opacity: combo.enabled ? 1 : 0.45
    }
    contentItem: Text {
        leftPadding: 11
        rightPadding: 32
        text: combo.displayText
        textFormat: Text.PlainText
        color: Theme.text
        font.pixelSize: Theme.fontBody
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Chevron {
        x: combo.width - width - 12
        anchors.verticalCenter: parent.verticalCenter
        down: true
        color: Theme.textSecondary
    }
    delegate: C.ItemDelegate {
        id: entry
        required property int index
        required property var modelData
        width: combo.popup.width - 8
        x: 4
        height: 36
        highlighted: combo.highlightedIndex === index
        background: Fill {
            color: entry.highlighted || entry.hovered ? Theme.subtleHover : Theme.subtleRest
            Rectangle {
                visible: combo.currentIndex === entry.index
                anchors.verticalCenter: parent.verticalCenter
                width: 3
                height: 16
                radius: 1.5
                color: Theme.accentFill
            }
        }
        contentItem: Text {
            leftPadding: 8
            text: typeof entry.modelData === "object" ? entry.modelData.text : entry.modelData
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.fontBody
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
    popup: C.Popup {
        y: combo.height + 2
        width: Math.max(combo.width, 160)
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 360)
        padding: 4
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: combo.popup.visible ? combo.delegateModel : null
            currentIndex: combo.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: C.ScrollBar { policy: C.ScrollBar.AsNeeded }
        }
        background: Rectangle {
            color: Theme.flyout
            radius: Theme.radiusOverlay
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 0.4)
        }
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.fast } }
    }
}
