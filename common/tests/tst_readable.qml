// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import Vela.Controls
import "colors.js" as Colors

// Theme.readableOn(): text over the accent stays the color Windows uses
// (white, or black on dark mode's light tint) while it reads there, and
// turns to the other one on a custom accent too light or too dark for it:
// the taskbar's notification count, today in the calendar, accent buttons.
TestCase {
    name: "Readable"

    function test_keeps_the_preferred_text_where_it_reads() {
        verify(Colors.same(Theme.readableOn("#0078d4", "white"), Qt.color("#ffffff")))
        verify(Colors.same(Theme.readableOn("#5b8cff", "white"), Qt.color("#ffffff")))
        verify(Colors.same(Theme.readableOn("#9cbcff", "black"), Qt.color("#000000")))
    }

    function test_turns_on_accents_too_light_or_too_dark() {
        // White on white or yellow, black on black or navy: unreadable.
        verify(Colors.same(Theme.readableOn("#ffffff", "white"), Qt.color("#000000")))
        verify(Colors.same(Theme.readableOn("#ffb900", "white"), Qt.color("#000000")))
        verify(Colors.same(Theme.readableOn("#000000", "black"), Qt.color("#ffffff")))
        verify(Colors.same(Theme.readableOn("#101040", "#000000"), Qt.color("#ffffff")))
    }

    function test_accent_text_reads_on_the_accent_fill() {
        // Whatever the accent, the text drawn on its fill has 3:1 at least.
        const luminance = c => {
            const channel = v => v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
            const color = Qt.color(c)
            return 0.2126 * channel(color.r) + 0.7152 * channel(color.g) + 0.0722 * channel(color.b)
        }
        const a = luminance(Theme.accentFill), b = luminance(Theme.accentText)
        verify((Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05) >= 3)
    }
}
