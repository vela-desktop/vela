// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Templates as T
import Vela.Controls

// A tile: on (accent color) or off, with the label below. `split`: an
// arrow on the right opening its page (Wi-Fi); `menu`: the whole tile
// opens the page (Accessibility). Turning on fades to the accent, hover
// and press fade; a right click opens its Settings page. The expressive
// variant draws QsTile instead, with the same properties and signals.
Item {
    id: tile
    property string icon
    property string label
    property bool checked
    property bool usable: true
    property bool split: false
    property bool menu: false
    signal toggled()
    signal details()
    signal contextMenu()
    // The quick settings' style; the tests pass their own.
    property QtObject style: Styles.quickSettings
    // The expressive tile's width (half the panel's content, QuickSettings).
    property real expressiveWidth: 160
    readonly property bool expressive: style.expressive
    width: expressive ? expressiveWidth : 104
    height: expressive ? 56 : 82
    opacity: usable || expressive ? 1 : 0.45 // QsTile fades itself

    // One part of a tile (both halves of a split one): Qt's button, the
    // accent when on, every change of color faded (Fill).
    component TileButton: T.AbstractButton {
        id: half
        property bool on
        // The tile's corners (style.tileRadius), passed by the tile.
        property int corner
        property bool roundLeft: true
        property bool roundRight: true
        // An inline arrow's half lights the whole tile: its hover and press
        // show on the other half, and it draws nothing itself.
        property bool lit: false
        property bool litDown: false
        property bool bare: false
        height: parent.height
        z: visualFocus ? 1 : 0 // the ring above the other half
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        // What a screen reader hears is Qt's own check state: Qt's button writes
        // its checkable and checked over the attached Accessible ones whenever
        // accessibility turns on. So a half that toggles is a checkable button
        // following the tile (`on`) that never toggles itself: Qt's toggle,
        // before `clicked`, is put back to the tile's state, which the tile's
        // owner then changes, or not (a refused Wi-Fi switch). Role Button: a
        // checkable button is a toggle button to assistive technology.
        checked: checkable && on
        onToggled: checked = Qt.binding(() => half.checkable && half.on)
        Accessible.role: Accessible.Button
        readonly property bool shownDown: down || litDown
        readonly property bool shownHovered: hovered || lit
        // bare: no fill of its own, but the focus ring still shows.
        background: Fill {
            topLeftRadius: half.roundLeft ? half.corner : 0
            bottomLeftRadius: half.roundLeft ? half.corner : 0
            topRightRadius: half.roundRight ? half.corner : 0
            bottomRightRadius: half.roundRight ? half.corner : 0
            color: half.bare ? "transparent"
                 : half.on ? (half.shownDown ? Qt.tint(Theme.accent, Qt.rgba(1, 1, 1, 0.06))
                              : half.shownHovered ? Qt.tint(Theme.accent, Qt.rgba(1, 1, 1, 0.08)) : Theme.accent)
                 : (half.shownDown ? Theme.controlPressed : half.shownHovered ? Theme.controlHover : Theme.control)
            FocusRing { visible: half.visualFocus }
        }
    }

    Loader {
        active: tile.expressive
        anchors.fill: parent
        sourceComponent: QsTile {
            icon: tile.icon
            label: tile.label
            checked: tile.checked
            usable: tile.usable
            split: tile.split
            menu: tile.menu
            squareRadius: tile.style.tileRadius
            onToggled: tile.toggled()
            onDetails: tile.details()
            onContextMenu: tile.contextMenu()
        }
    }

    Item {
        id: face
        visible: !tile.expressive
        width: parent.width
        height: 48
        // A style's arrow beside the icon (quicksettings-inline-arrow): the
        // tile is one button, its right half opens the page.
        readonly property bool inline: tile.style.inlineArrow
        readonly property real detailsWidth: inline ? width / 2 : 36

        TileButton {
            id: mainButton
            objectName: "mainButton"
            width: tile.split && !face.inline ? parent.width - 36 : parent.width
            on: tile.checked
            corner: tile.style.tileRadius
            checkable: !tile.menu
            roundRight: !tile.split || face.inline
            lit: detailsButton.hovered
            litDown: detailsButton.down
            enabled: tile.usable
            Accessible.name: tile.label
            onClicked: tile.menu ? tile.details() : tile.toggled()
        }
        TileButton {
            id: detailsButton
            objectName: "detailsButton"
            visible: tile.split
            x: parent.width - face.detailsWidth
            width: face.detailsWidth
            on: tile.checked
            corner: tile.style.tileRadius
            roundLeft: false
            bare: face.inline
            enabled: tile.usable
            Accessible.name: qsTr("%1 settings").arg(tile.label)
            onClicked: tile.details()
        }
        // Off: a thin border, which fades away as the tile turns on. A
        // style without borders keeps only a line along the bottom edge.
        Rectangle {
            visible: !tile.style.borderless
            anchors.fill: parent
            radius: tile.style.tileRadius
            color: "transparent"
            border.width: 1
            border.color: tile.checked ? Qt.rgba(Theme.stroke.r, Theme.stroke.g, Theme.stroke.b, 0) : Theme.stroke
            Behavior on border.color { ColorFade {} }
        }
        GlassEdge {
            atBottom: true
            cornerRadius: tile.style.tileRadius
            visible: tile.style.borderless && !tile.checked
        }
        Fill {
            visible: tile.split && !face.inline
            x: parent.width - 36
            y: 10
            width: 1
            height: parent.height - 20
            radius: 0
            color: tile.checked ? Qt.rgba(1, 1, 1, 0.35) : Theme.stroke
        }
        Icon {
            x: (tile.split || tile.menu) && face.inline ? parent.width / 2 - width - 2
                : (tile.split ? parent.width - 36 : parent.width) / 2 - width / 2
            anchors.verticalCenter: parent.verticalCenter
            name: tile.icon
            size: 18
            onAccent: tile.checked
        }
        Chevron {
            visible: tile.split || tile.menu
            x: face.inline ? parent.width / 2 + 6 : tile.split ? parent.width - 21 : parent.width - 18
            anchors.verticalCenter: parent.verticalCenter
            color: tile.checked ? "white" : Theme.text
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            enabled: tile.usable
            onTapped: tile.contextMenu()
        }
    }
    Text {
        visible: !tile.expressive
        anchors { top: face.bottom; topMargin: 6; horizontalCenter: parent.horizontalCenter }
        width: parent.width
        text: tile.label
        color: Theme.text
        font.pixelSize: Theme.fontCaption
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
    }
}
