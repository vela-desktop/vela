// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_OUTPUT_CONFIG_H
#define VELA_OUTPUT_CONFIG_H

// The output configuration the user chose (with a wlr-output-management
// program, or with Vela's Settings), remembered across sessions in
// ~/.config/vela/outputs.conf:
//
//   [Make Model Serial]
//   enabled=yes
//   mode=2560x1440@180.000
//   scale=1.25
//   rotation=normal
//   position=0,0
//
// Each monitor is recognized by make, model and serial number, not by
// connector: moving it to another port keeps its settings. Until October 2026
// the file was schermi.conf, in Italian: it's still read, and becomes
// outputs.conf at the first write.

#include <stdbool.h>

// An output as the user left it. transform is a wl_output_transform (0 normal
// ... 7 flipped and rotated by 270°).
struct vela_saved_output {
    bool enabled;
    int width; // 0: mode not saved
    int height;
    int refresh_mhz;
    float scale; // 0: not saved
    int transform;
    bool has_position;
    int x, y;
};

// Monitor `key` as the user left it; false if it isn't in the file.
bool vela_output_config_load(const char *key, struct vela_saved_output *out);

// Writes the current configuration of these outputs, leaving the other
// monitors' (unplugged now) as they were. For an output that is off only
// enabled=no is written: the rest stays as before.
struct vela_output_config_entry {
    const char *key;
    bool enabled;
    int width, height, refresh_mhz;
    float scale;
    int transform;
    int x, y;
};
// False when the configuration cannot be written and atomically replaced.
bool vela_output_config_save(const struct vela_output_config_entry *entries, int count);

#endif
