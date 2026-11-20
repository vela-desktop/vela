// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_OUTPUT_ARRANGEMENT_H
#define VELA_OUTPUT_ARRANGEMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vela_output_rect {
    int64_t x, y, width, height;
};

bool vela_output_rects_overlap(struct vela_output_rect a, struct vela_output_rect b);

// Start with the first output and retain positions wherever possible. Move
// disconnected or overlapping outputs to the nearest usable shared edge.
// The caller supplies zeroed scratch flags and normalizes the final origin.
void vela_output_arrange_rects(struct vela_output_rect *rects, bool *placed, size_t count);

#endif
