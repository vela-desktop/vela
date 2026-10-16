// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Where a key takes the selection among sections of tiles in rows of
// `columns` (Start: the pinned apps, then all apps). `counts` is each
// section's number of tiles. Left and right go round the section; up and
// down keep the column, from one non-empty section to the next and round.
// Returns { section, index }.
.pragma library

function next(counts, section, index, direction, columns) {
    const n = counts[section]
    if (direction === "left" || direction === "right")
        return { section: section, index: (index + (direction === "right" ? 1 : n - 1)) % n }
    if (direction === "down" && index + columns < n)
        return { section: section, index: index + columns }
    if (direction === "up" && index >= columns)
        return { section: section, index: index - columns }
    let s = section
    do {
        s = (s + (direction === "down" ? 1 : counts.length - 1)) % counts.length
    } while (counts[s] === 0)
    const m = counts[s]
    const row = direction === "down" ? 0 : Math.floor((m - 1) / columns) * columns
    return { section: s, index: Math.min(row + index % columns, m - 1) }
}
