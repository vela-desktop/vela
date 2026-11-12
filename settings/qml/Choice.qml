// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic as C
import Vela.Controls

// The Windows 11 drop-down (ComboBox). `model` is a list of strings or of
// objects with `text`; `currentIndex` the chosen one.
//
// Expressive: M3's exposed drop-down menu: a filled field (surface container
// highest, extra small top corners, the indicator at the bottom, a state
// layer of on surface), 40 high as TextBox (Vela's choice: no floating
// label); its menu without outline, the chosen entry in tertiary container
// (M3's vertical menu), the others state layers.
C.ComboBox {
    id: combo
    property bool usable: true
    property string styleVariant: Theme.controlsVariant
    readonly property bool expressive: styleVariant === "expressive"
    function stateLayer(amount) {
        return Qt.rgba(Theme.surfaceText.r, Theme.surfaceText.g, Theme.surfaceText.b, amount)
    }
    signal chosen(int index)

    enabled: usable
    hoverEnabled: true // not dependent on platform style hints (also works offscreen)
    // Disabled, the whole control fades, text and chevron too, like Button,
    // Switch and Slider: a drop-down that only lost its background read as
    // one that works.
    opacity: enabled ? 1 : 0.45
    implicitWidth: Math.max(160, Math.min(320, contentWidth + 56))
    implicitHeight: expressive ? 40 : 32
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
        readonly property real amount: combo.pressed ? Theme.statePressed : combo.hovered ? Theme.stateHover : 0
        radius: combo.expressive ? 0 : Theme.radius
        topLeftRadius: combo.expressive ? Theme.shapeExtraSmall : Theme.radius
        topRightRadius: combo.expressive ? Theme.shapeExtraSmall : Theme.radius
        color: combo.expressive ? Theme.mix(Theme.surfaceContainerHighest, Theme.surfaceText, amount)
            : combo.pressed ? Theme.controlPressed : combo.hovered ? Theme.controlHover : Theme.control
        border.width: combo.expressive ? 0 : 1
        border.color: Theme.controlStroke
        Rectangle {
            objectName: "indicator"
            visible: combo.expressive
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: combo.popup.visible || combo.visualFocus ? 2 : 1
            color: combo.popup.visible || combo.visualFocus ? Theme.primary : Theme.surfaceVariantText
        }
        FocusRing { visible: combo.expressive && combo.visualFocus; expressive: true }
    }
    contentItem: Text {
        leftPadding: combo.expressive ? 16 : 11
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
        readonly property bool chosen: combo.currentIndex === index
        background: Fill {
            readonly property real amount: entry.down ? Theme.statePressed : entry.highlighted || entry.hovered ? Theme.stateHover : 0
            radius: !combo.expressive ? Theme.radius : entry.chosen ? Theme.shapeMedium : Theme.shapeExtraSmall
            color: combo.expressive
                ? (entry.chosen ? Theme.mix(Theme.tertiaryContainer, Theme.tertiaryContainerText, amount) : combo.stateLayer(amount))
                : entry.highlighted || entry.hovered ? Theme.subtleHover : Theme.subtleRest
            Rectangle {
                visible: !combo.expressive && combo.currentIndex === entry.index
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
            color: combo.expressive && entry.chosen ? Theme.tertiaryContainerText : Theme.text
            font.weight: combo.expressive && entry.chosen ? Theme.weightLabel : Font.Normal
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
            objectName: "choicePopup"
            color: Theme.flyout
            radius: Theme.radiusOverlay
            border.width: combo.expressive ? 0 : 1
            border.color: Qt.rgba(0, 0, 0, 0.4)
        }
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.fast } }
    }
}
