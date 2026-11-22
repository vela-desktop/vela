// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Where characters as people see them start and end in a string, for the
// try field's Backspace, Delete and arrows: an emoji is one, not two UTF-16
// halves; an accent typed after its letter (e + U+0301), a variation
// selector, a skin tone and a ZWJ sequence (family emoji) go with what is
// before them. Close to Unicode's grapheme clusters, without their tables.
.pragma library

function isHigh(unit) {
    return unit >= 0xd800 && unit <= 0xdbff
}

function isLow(unit) {
    return unit >= 0xdc00 && unit <= 0xdfff
}

// The start of the code point that ends at `i`.
function codePointBefore(text, i) {
    return i >= 2 && isLow(text.charCodeAt(i - 1)) && isHigh(text.charCodeAt(i - 2)) ? i - 2 : i - 1
}

// The end of the code point that starts at `i`.
function codePointAfter(text, i) {
    return i + 1 < text.length && isHigh(text.charCodeAt(i)) && isLow(text.charCodeAt(i + 1)) ? i + 2 : i + 1
}

const ZWJ = 0x200d

// A code point that belongs to the one before it.
function joinsPrevious(code) {
    return (code >= 0x0300 && code <= 0x036f) // combining diacritical marks
        || (code >= 0x1ab0 && code <= 0x1aff) || (code >= 0x1dc0 && code <= 0x1dff)
        || (code >= 0x20d0 && code <= 0x20ff) || (code >= 0xfe20 && code <= 0xfe2f)
        || (code >= 0xfe00 && code <= 0xfe0f) // variation selectors
        || (code >= 0x1f3fb && code <= 0x1f3ff) // skin tones
        || (code >= 0xe0020 && code <= 0xe007f) // tags (subdivision flags)
        || (code >= 0xe0100 && code <= 0xe01ef) || code === ZWJ
}

// The boundary before `i`.
function previous(text, i) {
    if (i <= 0) {
        return 0
    }
    let p = codePointBefore(text, Math.min(i, text.length))
    while (p > 0) {
        if (joinsPrevious(text.codePointAt(p))) {
            p = codePointBefore(text, p)
            continue
        }
        const before = codePointBefore(text, p)
        if (text.codePointAt(before) === ZWJ && before > 0) {
            p = codePointBefore(text, before) // what the ZWJ joins
            continue
        }
        break
    }
    return p
}

// The boundary after `i`.
function next(text, i) {
    if (i >= text.length) {
        return text.length
    }
    let p = codePointAfter(text, Math.max(i, 0))
    while (p < text.length && joinsPrevious(text.codePointAt(p))) {
        const joiner = text.codePointAt(p) === ZWJ
        p = codePointAfter(text, p)
        if (joiner && p < text.length) {
            p = codePointAfter(text, p) // what the ZWJ joins
        }
    }
    return p
}
