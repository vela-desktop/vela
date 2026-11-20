// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output_arrangement.h"

#include <stdlib.h>

bool vela_output_rects_overlap(struct vela_output_rect a, struct vela_output_rect b)
{
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

static int64_t clamp(int64_t value, int64_t min, int64_t max)
{
    return value < min ? min : value > max ? max : value;
}

static bool position_clear(
    struct vela_output_rect candidate, const struct vela_output_rect *rects, const bool *placed, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (placed[i] && vela_output_rects_overlap(candidate, rects[i])) {
            return false;
        }
    }
    return true;
}

void vela_output_arrange_rects(struct vela_output_rect *rects, bool *placed, size_t count)
{
    if (!count) {
        return;
    }
    placed[0] = true;
    for (size_t remaining = count - 1; remaining > 0; --remaining) {
        int64_t best_distance = INT64_MAX;
        size_t best_index = 0;
        struct vela_output_rect best = { 0 };
        for (size_t i = 1; i < count && best_distance != 0; ++i) {
            if (placed[i]) {
                continue;
            }
            struct vela_output_rect a = rects[i];
            for (size_t j = 0; j < count && best_distance != 0; ++j) {
                if (!placed[j]) {
                    continue;
                }
                struct vela_output_rect b = rects[j];
                // Require a shared edge of at least one logical pixel.
                // Touching only at a corner leaves the pointer stranded.
                int64_t x = clamp(a.x, b.x - a.width + 1, b.x + b.width - 1);
                int64_t y = clamp(a.y, b.y - a.height + 1, b.y + b.height - 1);
                struct vela_output_rect candidates[] = {
                    { b.x - a.width, y, a.width, a.height },
                    { b.x + b.width, y, a.width, a.height },
                    { x, b.y - a.height, a.width, a.height },
                    { x, b.y + b.height, a.width, a.height },
                };
                for (size_t k = 0; k < sizeof(candidates) / sizeof(candidates[0]); ++k) {
                    struct vela_output_rect candidate = candidates[k];
                    int64_t distance = llabs(candidate.x - a.x) + llabs(candidate.y - a.y);
                    if (distance < best_distance && position_clear(candidate, rects, placed, count)) {
                        best_distance = distance;
                        best_index = i;
                        best = candidate;
                    }
                }
            }
        }
        // The right edge of the rightmost placed rectangle always has room.
        rects[best_index] = best;
        placed[best_index] = true;
    }
}
