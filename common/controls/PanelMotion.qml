// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// What every panel does the same way (docs/visual-coherence.md §6.9): it
// enters, leaves, turns back when reopened during its exit, and takes no
// input while leaving, neither the mouse nor the keys. A container: the owner puts the panel and its Shadow
// inside, places it where the panel rests, and the container moves, scales
// and fades them together. What differs per panel stays with it: where it
// comes from (`offset`, in units, and `scaleFrom`, both times Theme.motion,
// so with animation effects off only the opacity changes), how long its
// entrance moves (`duration`), and what `closed()` does.
//
// The fade in always takes `fast`, the exit always `normal`; menus, previews,
// Alt+Tab and Task View vanish at once (`instantExit`, K5).
Item {
    id: motion

    property point offset: Qt.point(0, 0)
    property real scaleFrom: 1
    property int duration: Theme.slow
    property bool instantExit: false
    // Leaving: the content takes no input.
    readonly property bool closing: exit.running
    // Showing or entering, not leaving.
    readonly property bool opened: d.opened
    // Entering or leaving.
    readonly property bool moving: enter.running || exit.running
    // The blur region, in the parent's coordinates. While the panel moves,
    // the rectangle it covers along its whole travel, so the region never
    // changes during a slide; at rest the panel itself, so its Shadow (in the
    // same surface, and the compositor blurs under any of its alpha) is not
    // blurred. Two changes per entrance, none per frame.
    readonly property rect area: moving ? travel : Qt.rect(x, y, width, height)
    // `area` cut to the parent (the window, where the panel rises from
    // behind the taskbar): what a panel sends as its blur region.
    readonly property rect visibleArea: {
        const a = area
        const w = parent ? parent.width : 0
        const h = parent ? parent.height : 0
        const left = Math.max(0, a.x)
        const top = Math.max(0, a.y)
        return Qt.rect(left, top, Math.max(0, Math.min(a.x + a.width, w) - left),
                       Math.max(0, Math.min(a.y + a.height, h) - top))
    }
    readonly property rect travel: {
        const dx = offset.x * Theme.motion
        const dy = offset.y * Theme.motion
        const grow = Math.max(0, scaleFrom - 1) * Theme.motion
        const gx = width * grow / 2
        const gy = height * grow / 2
        return Qt.rect(x + Math.min(0, dx) - gx, y + Math.min(0, dy) - gy,
                       width + Math.abs(dx) + 2 * gx, height + Math.abs(dy) + 2 * gy)
    }
    // 0 at rest, 1 where the panel comes from.
    property real away: 1
    signal closed()

    opacity: 0
    transform: [
        Scale {
            origin.x: motion.width / 2
            origin.y: motion.height / 2
            xScale: 1 + (motion.scaleFrom - 1) * Theme.motion * motion.away
            yScale: xScale
        },
        Translate {
            x: motion.offset.x * Theme.motion * motion.away
            y: motion.offset.y * Theme.motion * motion.away
        }
    ]

    // From where it is: from the start when closed, from the current offset
    // and opacity during an exit.
    function open() {
        if (d.opened)
            return
        d.opened = true
        const turningBack = exit.running
        exit.stop()
        if (!turningBack) {
            away = 1
            opacity = 0
        }
        enter.restart()
        // The keys go back where they were before the exit took them.
        if (turningBack && d.focus)
            d.focus.forceActiveFocus()
        d.focus = null
    }
    // `atOnce`: without the exit, when what comes next happens elsewhere at
    // once (the clipboard's paste needs the keyboard back in the app).
    function close(atOnce) {
        if (!d.opened)
            return
        d.opened = false
        enter.stop()
        if (instantExit || atOnce) {
            exit.stop()
            opacity = 0
            away = 1
            closed()
            return
        }
        // The keys too: a second Enter while a dialog leaves must not choose
        // again.
        const focused = Window.activeFocusItem
        d.focus = inside(focused) ? focused : null
        exit.restart()
        if (d.focus)
            blocker.forceActiveFocus()
    }

    function inside(item) {
        for (let i = item; i; i = i.parent) {
            if (i === motion)
                return true
        }
        return false
    }

    QtObject {
        id: d
        property bool opened: false
        property Item focus: null // what had the keys when the exit began
    }

    ParallelAnimation {
        id: enter
        Eased { target: motion; property: "away"; to: 0; duration: motion.duration }
        Eased { target: motion; property: "opacity"; to: 1; duration: Theme.fast }
    }
    ParallelAnimation {
        id: exit
        Eased { target: motion; property: "away"; to: 1 }
        Eased { target: motion; property: "opacity"; to: 0 }
        onFinished: motion.closed() // open() stops it: never after a reopening
    }

    // Above the content: while leaving, a click, the wheel, a hover or a key
    // reaches nothing.
    MouseArea {
        id: blocker
        z: 1000000
        anchors.fill: parent
        visible: motion.closing
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: {}
        Keys.onPressed: event => event.accepted = true
        Keys.onReleased: event => event.accepted = true
    }
}
