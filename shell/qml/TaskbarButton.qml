// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import Vela.Controls

// A taskbar button: the background fades on hover and a slight "bounce" when
// pressed. Under the icon, like on Windows 11, a gray dash if the app is open,
// longer and colored if it's the active one: on the side of the screen's
// edge, wherever the taskbar is (docs/taskbar.md §11.2). With `draggable` it
// can be dragged along the bar to move it (dropped says by how much).
Item {
    id: root

    property string tooltip: ""
    readonly property bool hovered: mouse.containsMouse
    property bool active: false
    property string shortcutNumber: ""
    property string countBadge: ""
    property real progressValue: -1
    property bool dropHighlighted: false
    property bool needsAttention: false
    property bool flashAttention: false
    property bool attentionLit: true

    function updateAttentionPulse() {
        attentionPulse.stop()
        attentionLit = true
        if (needsAttention && flashAttention) attentionPulse.start()
    }
    onNeedsAttentionChanged: updateAttentionPulse()
    onFlashAttentionChanged: updateAttentionPulse()

    SequentialAnimation {
        id: attentionPulse
        loops: 3
        PauseAnimation { duration: Theme.attentionPulseInterval }
        PropertyAction { target: root; property: "attentionLit"; value: false }
        PauseAnimation { duration: Theme.attentionPulseInterval }
        PropertyAction { target: root; property: "attentionLit"; value: true }
    }
    property bool running: false
    property bool draggable: false
    readonly property bool dragging: mouse.dragging
    default property alias content: holder.data

    signal clicked()
    signal middleClicked()
    signal rightClicked(bool shift) // Shift+right click: the window menu
    signal dropped(real dx)
    signal wheeled(real delta) // angleDelta.y

    readonly property QtObject style: Styles.taskbar
    readonly property bool vertical: style.vertical
    readonly property string edge: style.position

    // While dragged it follows the mouse, above the other buttons.
    z: mouse.dragging ? 10 : 0
    transform: Translate {
        x: mouse.dragging && !root.vertical ? mouse.dragX : 0
        y: mouse.dragging && root.vertical ? mouse.dragX : 0
    }

    width: style.buttonWidth
    height: style.buttonHeight

    Fill {
        anchors.fill: parent
        radius: root.style.buttonRadius
        color: root.needsAttention && root.attentionLit ? Theme.attention : mouse.pressed ? Theme.subtlePressed : mouse.containsMouse || root.active || root.dropHighlighted ? Theme.subtleHover : Theme.subtleRest
    }

    Item {
        id: holder
        anchors.centerIn: parent
        width: root.style.iconSize
        height: root.style.iconSize
        scale: mouse.pressed ? 1 - 0.16 * Theme.motion : 1

        Behavior on scale { Eased {} }
    }

    // Indicator under the icon: app open / active (or Start menu open).
    // Expressive (Vela's choice, M3 has no taskbar): a 4 high pill, 24 wide
    // in primary when active, 8 in outline when open, whose width moves on
    // the fast spatial curve.
    Rectangle {
        objectName: "indicator"
        readonly property bool expressive: Theme.expressive
        readonly property real thick: expressive ? 4 : 3
        readonly property real long: root.active ? (expressive ? 24 : 16) : root.running ? (expressive ? 8 : 6) : 0
        x: root.edge === "left" ? 2 : root.edge === "right" ? parent.width - width - 2 : Math.round((parent.width - width) / 2)
        y: root.edge === "top" ? 2 : root.edge === "bottom" ? parent.height - height - 2 : Math.round((parent.height - height) / 2)
        width: root.vertical ? thick : long
        height: root.vertical ? long : thick
        radius: (root.vertical ? width : height) / 2
        color: root.active ? (expressive ? Theme.primary : Theme.accent) : (expressive ? Theme.outline : Theme.textSecondary)
        opacity: root.progressValue < 0 && (root.active || root.running) ? 1 : 0

        Behavior on width {
            enabled: Theme.motion > 0 && !root.vertical
            Spatial { speed: "fast"; rest: Theme.normal }
        }
        Behavior on height {
            enabled: Theme.motion > 0 && root.vertical
            Spatial { speed: "fast"; rest: Theme.normal }
        }
        Behavior on color { ColorFade {} }
        Behavior on opacity {
            NumberAnimation { duration: Theme.fast }
        }
    }

    // The progress track, where the dash is; filled from the top on a
    // vertical bar.
    Rectangle {
        visible: root.progressValue >= 0
        readonly property real thick: Theme.expressive ? 4 : 3
        x: root.edge === "left" ? 2 : root.edge === "right" ? parent.width - width - 2 : 4
        y: root.edge === "top" ? 2 : root.edge === "bottom" ? parent.height - height - 2 : 4
        width: root.vertical ? thick : parent.width - 8
        height: root.vertical ? parent.height - 8 : thick
        radius: (root.vertical ? width : height) / 2
        color: Theme.stroke
        Rectangle {
            width: root.vertical ? parent.width : parent.width * Math.max(0, root.progressValue)
            height: root.vertical ? parent.height * Math.max(0, root.progressValue) : parent.height
            radius: parent.radius
            color: Theme.accent
        }
    }

    // The count at the corner away from the screen's edge.
    Rectangle {
        id: count
        objectName: "countBadge"
        // Hidden while Super's number takes its place (below).
        visible: root.countBadge !== "" && !number.onCount
        x: root.edge === "right" ? 2 : parent.width - width - 2
        y: root.edge === "top" ? parent.height - height - 2 : 2
        width: Math.max(16, countLabel.implicitWidth + 6)
        height: 16
        radius: height / 2
        color: Theme.accentFill
        Text {
            id: countLabel
            anchors.centerIn: parent
            text: root.countBadge
            color: Theme.accentText
            font.pixelSize: 10
        }
    }

    // Super's number (docs/taskbar.md §7.4): the other corner away from the
    // screen's edge, clear of the open-app indicator and of the count. On a
    // button too small for both it shows in the count's place, for as long
    // as Super is held.
    Rectangle {
        id: number
        objectName: "shortcutNumber"
        visible: root.shortcutNumber !== ""
        readonly property real cornerX: root.edge === "left" ? parent.width - width - 3 : 3
        readonly property real cornerY: root.vertical || root.edge === "top" ? parent.height - height - 3 : 3
        readonly property bool onCount: visible && root.countBadge !== ""
            && cornerX < count.x + count.width + 1 && count.x < cornerX + width + 1
            && cornerY < count.y + count.height + 1 && count.y < cornerY + height + 1
        x: onCount ? count.x + count.width - width : cornerX
        y: onCount ? count.y : cornerY
        width: 16
        height: 16
        radius: height / 2
        color: Theme.accentFill
        Text {
            anchors.centerIn: parent
            color: Theme.accentText
            font.pixelSize: 10
            text: root.shortcutNumber
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        property real pressX: 0 // along the bar
        property real dragX: 0
        property bool dragging: false
        property bool dragged: false // releasing a drag isn't a click
        // In window coordinates: the button moves with the mouse, they don't.
        function alongOf(mouse) {
            const p = mapToItem(null, mouse.x, mouse.y)
            return root.vertical ? p.y : p.x
        }
        onPressed: mouse => {
            pressX = alongOf(mouse)
            dragX = 0
            dragged = false
        }
        onPositionChanged: mouse => {
            if (!pressed || !root.draggable || !(pressedButtons & Qt.LeftButton)) return
            dragX = alongOf(mouse) - pressX
            if (!dragging && Math.abs(dragX) > 8) dragging = true
        }
        onReleased: mouse => {
            if (dragging) {
                dragging = false
                dragged = true
                root.dropped(dragX)
            }
        }
        onWheel: wheel => root.wheeled(wheel.angleDelta.y)
        onClicked: mouse => {
            if (dragged) {
                dragged = false
                return
            }
            if (mouse.button === Qt.MiddleButton) {
                root.middleClicked()
            } else if (mouse.button === Qt.RightButton) {
                root.rightClicked(mouse.modifiers & Qt.ShiftModifier)
            } else {
                root.clicked()
            }
        }
    }
}
