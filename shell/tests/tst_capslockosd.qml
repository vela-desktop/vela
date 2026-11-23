// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "../qml"

// The Caps Lock display (CapsLockOsd.qml, docs/keyboard.md §8.3): what the
// compositor's "caps-lock on|off" shows, what a screen reader hears, how
// long it stays, and the volume indicator taking its place.
Item {
    id: root
    width: 400
    height: 300

    CapsLockOsd { id: osd }

    TestCase {
        name: "CapsLockOsd"
        when: windowShown

        function cleanup() {
            tryVerify(() => !osd.visible, osd.hold + 5000)
        }

        function test_says_on_and_off() {
            Shell.capsLockChanged(true)
            verify(osd.visible)
            compare(Shell.placed[Shell.placed.length - 1], "test-output")
            const label = findChild(osd.contentItem, "capsLockLabel")
            const icon = findChild(osd.contentItem, "capsLockIcon")
            const pill = findChild(osd.contentItem, "capsLockPill")
            compare(label.text, "Caps Lock on")
            compare(icon.name, "keyboard-caps-locked")
            compare(pill.Accessible.role, Accessible.AlertMessage)
            compare(pill.Accessible.name, "Caps Lock on")
            Shell.capsLockChanged(false)
            compare(label.text, "Caps Lock off")
            compare(icon.name, "keyboard-caps-disabled")
            compare(pill.Accessible.name, "Caps Lock off")
            // Wider than its text, never narrower than 180.
            verify(pill.width >= 180)
            verify(pill.width >= label.implicitWidth + icon.width + 10)
        }

        // A second and a half at Vela's durations, then it leaves by itself.
        function test_fades_after_a_moment() {
            compare(osd.hold, Math.round(1500 * Theme.duration))
            Shell.capsLockChanged(true)
            wait(Math.round(osd.hold / 2))
            verify(osd.visible)
            // Another change starts the moment again.
            Shell.capsLockChanged(false)
            wait(Math.round(osd.hold * 0.75))
            verify(osd.visible)
            tryVerify(() => !osd.visible, osd.hold + 5000)
        }

        // The volume indicator appears where it is: it leaves.
        function test_volume_takes_the_place() {
            Shell.capsLockChanged(true)
            verify(osd.visible)
            Sound.osdRequested("")
            tryVerify(() => !osd.visible, 5000)
        }

        // The display the user is working on, as the compositor says: the
        // display appears there, not on the main one (the volume indicator,
        // which shares the place, does the same).
        function test_the_active_display() {
            Shell.activeScreen = "second-output"
            Shell.capsLockChanged(true)
            verify(osd.visible)
            compare(Shell.placed[Shell.placed.length - 1], "second-output")
            Shell.activeScreen = ""
            tryVerify(() => !osd.visible, osd.hold + 5000)
        }
    }
}
