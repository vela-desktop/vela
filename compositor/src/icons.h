// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_ICONS_H
#define VELA_ICONS_H

// App icons in the title bar (docs/renderer.md §9.6): from the app's .desktop
// file (Icon=) to KDE's icon theme (freedesktop spec), drawn at the exact
// physical size. SVGs through librsvg, always preferred; PNGs only when
// nothing else exists, scaled down with a quality filter. Without librsvg
// (optional) there are no icons.

#include "image.h"

#include <stdbool.h>

struct vela_icons;

// KDE's theme and its inheritance chain; .desktop files are read on the first
// request.
struct vela_icons *vela_icons_create(void);
void vela_icons_destroy(struct vela_icons *icons);

// The longest app_id that can have an icon. A .desktop id and an icon file's
// name are file names, at most NAME_MAX (255) bytes with their extension, so
// a longer app_id names neither; it is answered "none" without a search and
// without being remembered.
#define VELA_ICON_APP_ID_MAX 255

// The app's icon (Wayland app_id or X11 class), size x size pixels, copied
// into `out`: true, and `out` belongs to the caller (vela_image_finish, or
// handed on). false, and `out` empty, when none is found.
//
// The result is always a copy. `icons` remembers what it drew or failed to
// find in a cache with a fixed budget, VELA_ICON_CACHE_BUDGET entries, that
// evicts the least recently used entry (icon_cache.h); a copy stays valid
// when its entry is evicted, so a title bar keeps its icon on screen, and
// asking again for an evicted icon searches and draws it again.
bool vela_icons_app(struct vela_icons *icons, const char *app_id, int size, struct vela_image *out);

// The entries now in the cache, misses included: never above
// VELA_ICON_CACHE_BUDGET. For the tests.
int vela_icons_cached(const struct vela_icons *icons);

#endif
