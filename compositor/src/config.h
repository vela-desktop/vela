// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_CONFIG_H
#define VELA_CONFIG_H

// Vela's settings that don't concern outputs, in ~/.config/vela/vela.conf:
// key=value lines (# for comments). The Settings app writes them, then sends
// "reload-config" to the compositor.
//
//   screen-off=10            minutes of inactivity (0: never)
//   lock-on-idle=yes         lock before turning off
//   keyboard-layout=it,us    XKB layouts (Win+Space switches to the next)
//   keyboard-variant=,
//   keyboard-options=
//   keyboard-repeat-delay=400  ms before a held key repeats
//   keyboard-repeat-rate=30  repeats per second
//   night-light=no           night light on (quick settings and the schedule
//                            change it too)
//   night-light-temperature=4200  kelvin, 1700-6500 (night-light-strength=0-100
//                            before it, still read without it)
//   night-light-nvidia-fix=no  experiment: render Night light in Vulkan instead of DRM gamma
//   night-light-schedule=no|sunset|hours
//   night-light-from=21:00, night-light-to=07:00
//   color-filters=no         color filter on
//   color-filter=grayscale   grayscale, deuteranopia, protanopia, tritanopia
//   color-filters-shortcut=no  Win+Ctrl+C turns it on and off
//   magnifier-step=100       how much Win+plus zooms in (percent)
//   sticky-keys=no           Shift, Ctrl, Alt and Win stay pressed
//   animation-effects=yes    windows move as they open, close, minimize...
//                            (no: they only fade, briefly)
//   animation-duration=100   50-300: percent of Vela's animation durations
//   tearing=yes              for fullscreen games that ask for it
//   direct-scanout=auto      auto, yes (the same) or no: never put a fullscreen
//                            app's buffer straight on screen (black screen or
//                            glitches in a game); VELA_SCANOUT=0 wins
//   variable-refresh=games   VRR: no, games (fullscreen apps), always
//   mouse-speed=10           1-20, like the Windows pointer
//   mouse-precision=yes      pointer acceleration
//   mouse-primary-button=left|right
//   mouse-scroll-lines=3     lines per wheel notch
//   touchpad=yes, touchpad-with-mouse=yes (stays on with a mouse too)
//   touchpad-speed=10, touchpad-tap=yes, touchpad-natural-scroll=yes
//   touchpad-three-fingers=app  gestures: app, desktop, no
//   touchpad-four-fingers=desktop
//   language=                it, en, or empty (like the system)
//   polkit-agent=yes         start Vela's polkit agent (docs/polkit-agent.md)
//   brightness=yes           drive the monitors' brightness (docs/brightness.md)
//
// The old Italian names are still read (legacy_names.h).

#include <stdbool.h>
#include <stddef.h>

// The file as read: one entry per key (for repeated keys the last line wins).
// Each entry is a single allocation, "key\0value".
struct vela_config_entry {
    char *key;
    const char *value; // inside the same allocation as key
};

struct vela_config {
    struct vela_config_entry *entries;
    int count;
    int capacity;
};

// Reads vela.conf (a missing file gives an empty list). Free with
// vela_config_finish.
void vela_config_read(struct vela_config *config);
void vela_config_finish(struct vela_config *config);

// The key's value, or `fallback` if it's missing.
const char *vela_config_get(const struct vela_config *config, const char *key, const char *fallback);
// "yes"/"1"/"true"; `fallback` if missing or empty.
bool vela_config_flag(const struct vela_config *config, const char *key, bool fallback);

// Changes one key in the file and leaves the rest as it is (comments too): for
// what is also turned on outside Settings (quick settings, shortcuts).
void vela_config_write(const char *key, const char *value);

// Rewrites vela.conf with the English names of keys and values if it still has
// the old Italian ones. At startup.
void vela_config_migrate(void);

// $XDG_CONFIG_HOME/vela/<name> (or ~/.config/vela/<name>); name NULL: the
// directory. false if the directory is unknown or the path doesn't fit in
// `size`.
bool vela_config_path(const char *name, char *out, size_t size);

#endif
