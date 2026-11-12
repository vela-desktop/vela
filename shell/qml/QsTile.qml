// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
import Vela.Controls

// A quick settings tile of the expressive variant (quicksettings-variant=
// expressive, QuickSettings.qml): wide, its icon and label inside, two to a
// row. Off: a full pill on surface container high; on: primary, its corners
// squared to `squareRadius` (Styles.quickSettings.tileRadius), the radius morphing on M3's
// fast spatial curve (Spatial), and to M3's pressed shape while pressed. It
// behaves as the default look's tile: `split`, an arrow at the trailing end
// opening the tile's page; `menu`, the whole tile opens it; a right click
// asks for its Settings page; Return and Enter press the focused half; the
// same accessible names and check state.
//
// From M3 (component-tokens.md, ButtonMedium, a medium toggle button): the
// 56 height, round when unselected, the square shape when selected
// (CornerLarge, 16), the pressed shape (CornerMedium, 12), the 8 between
// icon and label; the state layers (Theme.stateHover, statePressed); the
// label in label large's weight (Medium).
// Vela's choices (M3 specifies no desktop shell): the width (half the
// panel), the 20 icon and 16 leading space for the desktop's density, the
// 44 trailing arrow of a split tile with its state layer of its own, the
// disabled opacity as the default tile's.
Item {
    id: tile
    property string icon
    property string label
    property bool checked
    property bool usable: true
    property bool split: false
    property bool menu: false
    // The square corners when on: Styles.quickSettings.tileRadius, which
    // QuickSettings passes.
    property real squareRadius: 16
    signal toggled()
    signal details()
    signal contextMenu()
    implicitWidth: 160
    implicitHeight: 56
    opacity: usable ? 1 : 0.45

    // The corner the tile rests at, and the one it shows while it morphs.
    readonly property real restRadius: mainButton.down || detailsButton.down ? Theme.shapeMedium
        : checked ? squareRadius : height / 2
    property real shownRadius: restRadius
    Behavior on shownRadius {
        enabled: Theme.motion > 0
        Spatial { speed: "fast"; rest: Theme.fast }
    }
    // The overshoot stays inside the shape: never past round or below square.
    readonly property real radius: Math.max(0, Math.min(height / 2, shownRadius))

    readonly property color content: checked ? Theme.primaryText : Theme.surfaceText
    readonly property real detailsWidth: split ? 44 : 0

    function stateColor(button) {
        const alpha = button.down ? Theme.statePressed : button.hovered ? Theme.stateHover : 0
        return Qt.rgba(content.r, content.g, content.b, alpha)
    }

    Fill {
        objectName: "tileShape"
        anchors.fill: parent
        radius: tile.radius
        color: tile.checked ? Theme.primary : Theme.surfaceContainerHigh
    }
    // The state layers, one per half, with the shape's outer corners.
    Fill {
        objectName: "mainLayer"
        width: mainButton.width
        height: parent.height
        radius: 0
        topLeftRadius: tile.radius
        bottomLeftRadius: tile.radius
        topRightRadius: tile.split ? 0 : tile.radius
        bottomRightRadius: tile.split ? 0 : tile.radius
        color: tile.stateColor(mainButton)
    }
    Fill {
        visible: tile.split
        x: mainButton.width
        width: tile.detailsWidth
        height: parent.height
        radius: 0
        topRightRadius: tile.radius
        bottomRightRadius: tile.radius
        color: tile.stateColor(detailsButton)
    }

    T.AbstractButton {
        id: mainButton
        objectName: "mainButton"
        width: parent.width - tile.detailsWidth
        height: parent.height
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        enabled: tile.usable
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        // Qt's own check state, as the default tile's TileButton
        // (QuickSettingsTile.qml): Qt writes it over the attached one once
        // accessibility is on. It follows the tile and never toggles itself.
        checkable: !tile.menu
        checked: checkable && tile.checked
        onToggled: checked = Qt.binding(() => mainButton.checkable && tile.checked)
        Accessible.role: Accessible.Button
        Accessible.name: tile.label
        onClicked: tile.menu ? tile.details() : tile.toggled()
        // The ring around the whole tile, with its corners.
        background: Item {
            readonly property real radius: tile.radius
            FocusRing {
                anchors.fill: undefined
                x: -3
                y: -3
                width: tile.width + 6
                height: tile.height + 6
                visible: mainButton.visualFocus
                expressive: true
            }
        }
    }
    T.AbstractButton {
        id: detailsButton
        objectName: "detailsButton"
        visible: tile.split
        x: mainButton.width
        width: tile.detailsWidth
        height: parent.height
        z: visualFocus ? 1 : 0
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        enabled: tile.usable
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        Accessible.name: qsTr("%1 settings").arg(tile.label)
        onClicked: tile.details()
        background: Item {
            readonly property real radius: Math.min(tile.radius, height / 2)
            FocusRing { visible: detailsButton.visualFocus; expressive: true }
        }
    }

    Icon {
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        name: tile.icon
        size: 20
        // On primary: white drawings on the light mode's dark primary, the
        // light mode's dark ones on the dark mode's light primary.
        source: name === "" ? "" : !tile.checked ? Theme.icons + encodeURIComponent(name)
            : "image://icon/" + (Theme.light ? "w/" : "l/") + encodeURIComponent(name)
    }
    Text {
        objectName: "tileLabel"
        x: 16 + 20 + 8
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width - x - (tile.split ? tile.detailsWidth : tile.menu ? 36 : 12)
        text: tile.label
        color: tile.content
        Behavior on color { ColorFade {} }
        font.pixelSize: Theme.fontBody
        font.weight: Font.Medium
        elide: Text.ElideRight
    }
    Chevron {
        visible: tile.split || tile.menu
        x: tile.split ? parent.width - tile.detailsWidth / 2 - width / 2 : parent.width - 16 - width
        anchors.verticalCenter: parent.verticalCenter
        color: tile.content
        Behavior on color { ColorFade {} }
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: tile.usable
        onTapped: tile.contextMenu()
    }
}
