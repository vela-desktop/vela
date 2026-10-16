// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Distances between colors, to catch a fade halfway.
.pragma library

// A color read from a property follows it: this one stays.
function copy(c) {
    return Qt.rgba(c.r, c.g, c.b, c.a)
}

function distance(a, b) {
    return Math.abs(a.r - b.r) + Math.abs(a.g - b.g) + Math.abs(a.b - b.b) + Math.abs(a.a - b.a)
}

function same(a, b) {
    return distance(a, b) < 0.004
}

// Strictly on the way from `from` to `to`: neither end.
function between(c, from, to) {
    const whole = distance(from, to)
    return whole > 0 && distance(c, from) > 0.004 && distance(c, to) > 0.004
        && distance(c, from) + distance(c, to) <= whole + 0.01
}
