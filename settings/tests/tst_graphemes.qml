// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtTest
import "../qml/pages/Graphemes.js" as Graphemes

// Graphemes.js: the try field's Backspace, Delete and arrows move by
// characters as people see them (KeyboardLayoutsPage.qml).
TestCase {
    name: "Graphemes"

    function test_letters() {
        compare(Graphemes.previous("abc", 3), 2)
        compare(Graphemes.next("abc", 1), 2)
        compare(Graphemes.previous("abc", 0), 0)
        compare(Graphemes.next("abc", 3), 3)
    }

    function test_an_emoji_is_one() {
        const text = "A\u{1F600}b" // A, grinning face (two UTF-16 units), b
        compare(text.length, 4)
        compare(Graphemes.previous(text, 3), 1)
        compare(Graphemes.next(text, 1), 3)
        // Backspace after it leaves "A".
        compare(text.slice(0, Graphemes.previous(text, 3)), "A")
    }

    function test_marks_go_with_their_letter() {
        const text = "éx" // e + combining acute
        compare(Graphemes.previous(text, 2), 0)
        compare(Graphemes.next(text, 0), 2)
        const heart = "❤️!" // heart + emoji presentation
        compare(Graphemes.next(heart, 0), 2)
        compare(Graphemes.previous(heart, 2), 0)
    }

    function test_skin_tones_and_zwj_sequences() {
        const wave = "\u{1F44B}\u{1F3FD}" // waving hand, medium skin tone
        compare(Graphemes.previous(wave, wave.length), 0)
        compare(Graphemes.next(wave, 0), wave.length)
        const family = "x\u{1F468}‍\u{1F469}‍\u{1F467}y" // man ZWJ woman ZWJ girl
        const start = 1
        const end = family.length - 1
        compare(Graphemes.previous(family, end), start)
        compare(Graphemes.next(family, start), end)
    }
}
