// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_ICON_CACHE_H
#define VELA_ICON_CACHE_H

// The icons already drawn, or looked up without success, by (app_id, size):
// what icons.c remembers so that a title bar does not search the icon theme
// on disk and draw an SVG again (docs/memory-allocation-policy.md, "Aggregate
// growth"). The key comes from clients (xdg_toplevel.set_app_id, the X11
// class), so the collection has a budget and an eviction rule.
//
// Budget: at most `budget` entries, hits and misses together. icons.c passes
// VELA_ICON_CACHE_BUDGET.
// Eviction: inserting into a full cache frees the least recently used entry
// first. "Used" is a successful vela_icon_cache_find or the insertion itself.
// Lookup: a hash table with chaining, sized once at creation; nothing in it
// grows afterwards.

#include "image.h"

// The entry budget of the compositor's cache.
//
// What a session needs: one entry per distinct (app, title-bar icon size),
// and the size is 16 units at the output's scale, so one entry per app and
// per distinct output scale it has been shown at. A busy session has some
// tens of apps with a title bar from Vela, on one to three scales: about a
// hundred entries at most. 256 leaves that session untouched, so eviction
// only ever happens under a client that keeps changing its app_id (or a very
// long session), where it costs one more search and drawing the next time
// that icon is asked for.
//
// What the budget costs: an entry is its key, at most VELA_ICON_APP_ID_MAX
// bytes (icons.h), and size x size ARGB pixels, which the compositor chooses:
// 1 KiB at 100%, 4 KiB at 200%, 16 KiB at 400%; a miss has no pixels. A full
// cache is therefore about 1 MiB of pixels at 200% and at most 64 KiB of
// keys, plus the table (512 pointers).
//
// Misses share the budget instead of having their own: a miss is the entry
// that saves the most work (the whole theme chain is searched before giving
// up) and costs the least memory, and one budget keeps one rule. The price
// is that a client cycling through unknown app_ids can push real icons out;
// they are then drawn again on their next request, never lost.
#define VELA_ICON_CACHE_BUDGET 256

struct vela_icon_cache;

// `budget` is at least 1.
struct vela_icon_cache *vela_icon_cache_create(int budget);
void vela_icon_cache_destroy(struct vela_icon_cache *cache);

// The image remembered for (app_id, size), empty (pixels NULL) for a
// remembered miss; NULL when nothing is remembered. Marks the entry as the
// most recently used. The pointer is valid only until the next
// vela_icon_cache_insert or vela_icon_cache_destroy, which may free it:
// whoever keeps the pixels copies them first.
const struct vela_image *vela_icon_cache_find(struct vela_icon_cache *cache, const char *app_id, int size);

// Remembers `image` (empty for a miss) for (app_id, size), taking its pixels
// over and leaving `image` empty; an entry with the same key is replaced.
// When the cache is full, the least recently used entry is freed first.
// Returns the stored image, valid as for vela_icon_cache_find.
const struct vela_image *vela_icon_cache_insert(struct vela_icon_cache *cache, const char *app_id, int size,
    struct vela_image *image);

// The entries now in the cache: never above the budget.
int vela_icon_cache_count(const struct vela_icon_cache *cache);

#endif
