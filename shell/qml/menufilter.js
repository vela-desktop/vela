// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

.pragma library

// The entries of a menu the user didn't hide (Settings > Personalization >
// Context menu, docs/renderer.md §14.12). An entry with a `key` among
// `hidden` goes, with its submenu; a submenu or icon row left empty goes
// too, and so do the separators and group titles left with nothing to
// separate or title. Entries without a key always stay. The entries aren't
// changed: those with children or an icon row are copied.
function visible(entries, hidden) {
    const out = []
    const drop = new Set(hidden || [])
    const kept = e => !(e.key && drop.has(e.key))
    for (const entry of entries || []) {
        if (!kept(entry)) {
            continue
        }
        if (entry.children) {
            const children = visible(entry.children, hidden)
            if (!children.some(selectable)) {
                continue
            }
            out.push(Object.assign({}, entry, { children: children }))
        } else if (entry.iconRow) {
            const row = entry.iconRow.filter(kept)
            if (row.length > 0) {
                out.push(row.length === entry.iconRow.length ? entry : Object.assign({}, entry, { iconRow: row }))
            }
        } else {
            out.push(entry)
        }
    }
    return tidy(out)
}

function selectable(entry) {
    return !entry.separator && entry.header === undefined
}

// No separator at either end or next to another; no title without an entry
// under it before the next title or separator.
function tidy(entries) {
    const out = []
    for (let i = 0; i < entries.length; ++i) {
        const e = entries[i]
        if (e.separator) {
            if (out.length > 0 && !out[out.length - 1].separator) {
                out.push(e)
            }
        } else if (e.header !== undefined) {
            const next = entries[i + 1]
            if (next && selectable(next)) {
                out.push(e)
            }
        } else {
            out.push(e)
        }
    }
    while (out.length > 0 && (out[out.length - 1].separator || out[out.length - 1].header !== undefined)) {
        out.pop()
    }
    return out
}

// Opened from the keyboard, an icon row (Cut, Copy, Rename...) becomes rows:
// the arrows and the letters reach them, as any other entry.
function iconRowsAsRows(entries) {
    const out = []
    for (const entry of entries) {
        if (!entry.iconRow) {
            out.push(entry)
            continue
        }
        for (const button of entry.iconRow) {
            out.push(Object.assign({}, button))
        }
        out.push({ separator: true })
    }
    return tidy(out)
}
