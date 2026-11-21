// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output_arrangement.h"
#include "alloc/alloc.h"

#include <stdlib.h>

bool vela_output_rects_overlap(struct vela_output_rect a, struct vela_output_rect b)
{
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

static int64_t clamp(int64_t value, int64_t min, int64_t max)
{
    return value < min ? min : value > max ? max : value;
}

static bool share_edge(struct vela_output_rect a, struct vela_output_rect b)
{
    return ((a.x + a.width == b.x || b.x + b.width == a.x) && a.y < b.y + b.height && b.y < a.y + a.height)
        || ((a.y + a.height == b.y || b.y + b.height == a.y) && a.x < b.x + b.width && b.x < a.x + a.width);
}

static size_t group_root(const size_t *groups, size_t i)
{
    while (groups[i] != i) {
        i = groups[i];
    }
    return i;
}

struct arrangement {
    struct vela_output_rect *rects;
    size_t *groups;
    bool *placed;
    size_t count;
};

static void find_groups(const struct arrangement *layout)
{
    bool overlapping = false;
    for (size_t i = 0; i < layout->count; ++i) {
        layout->groups[i] = i;
        for (size_t j = 0; j < i; ++j) {
            overlapping |= vela_output_rects_overlap(layout->rects[i], layout->rects[j]);
            if (share_edge(layout->rects[i], layout->rects[j])) {
                layout->groups[group_root(layout->groups, i)] = group_root(layout->groups, j);
            }
        }
    }
    for (size_t i = 0; i < layout->count; ++i) {
        // Overlapping saved rectangles can require changing relative positions.
        layout->groups[i] = overlapping ? i : group_root(layout->groups, i);
    }
}

static bool position_clear(const struct arrangement *layout, size_t moving, struct vela_output_rect candidate)
{
    int64_t dx = candidate.x - layout->rects[moving].x;
    int64_t dy = candidate.y - layout->rects[moving].y;
    for (size_t i = 0; i < layout->count; ++i) {
        if (layout->groups[i] != layout->groups[moving]) {
            continue;
        }
        struct vela_output_rect member = layout->rects[i];
        member.x += dx;
        member.y += dy;
        for (size_t j = 0; j < layout->count; ++j) {
            if (layout->placed[j] && vela_output_rects_overlap(member, layout->rects[j])) {
                return false;
            }
        }
    }
    return true;
}

// When a projection is blocked, try translations that align a member's edge
// with the edge of a placed display. Those are the bounds of the free intervals.
static bool closest_on_edge(const struct arrangement *layout, size_t moving, struct vela_output_rect *candidate,
    struct vela_output_rect anchor, bool vertical)
{
    if (position_clear(layout, moving, *candidate)) {
        return true;
    }
    struct vela_output_rect origin = layout->rects[moving];
    int64_t project = vertical ? candidate->y : candidate->x;
    int64_t start = vertical ? anchor.y : anchor.x;
    int64_t end = start + (vertical ? anchor.height : anchor.width);
    int64_t length = vertical ? candidate->height : candidate->width;
    int64_t best = INT64_MAX;
    struct vela_output_rect trial = *candidate;
    for (size_t i = 0; i < layout->count; ++i) {
        if (layout->groups[i] != layout->groups[moving]) {
            continue;
        }
        struct vela_output_rect member = layout->rects[i];
        int64_t offset = vertical ? member.y - origin.y : member.x - origin.x;
        int64_t member_length = vertical ? member.height : member.width;
        for (size_t j = 0; j < layout->count; ++j) {
            if (!layout->placed[j]) {
                continue;
            }
            struct vela_output_rect other = layout->rects[j];
            int64_t edge = (vertical ? other.y : other.x) - offset;
            int64_t positions[] = { edge - member_length, edge + (vertical ? other.height : other.width) };
            for (size_t k = 0; k < sizeof(positions) / sizeof(positions[0]); ++k) {
                int64_t pos = positions[k];
                int64_t distance = llabs(pos - project);
                if (pos <= start - length || pos >= end || distance >= best) {
                    continue;
                }
                if (vertical) {
                    trial.y = pos;
                } else {
                    trial.x = pos;
                }
                if (position_clear(layout, moving, trial)) {
                    *candidate = trial;
                    best = distance;
                }
            }
        }
    }
    return best != INT64_MAX;
}

static size_t place_group(struct arrangement *layout, size_t index, struct vela_output_rect position)
{
    int64_t dx = position.x - layout->rects[index].x;
    int64_t dy = position.y - layout->rects[index].y;
    size_t placed = 0;
    for (size_t i = 0; i < layout->count; ++i) {
        if (layout->groups[i] == layout->groups[index]) {
            layout->rects[i].x += dx;
            layout->rects[i].y += dy;
            layout->placed[i] = true;
            ++placed;
        }
    }
    return placed;
}

void vela_output_arrange_rects(struct vela_output_rect *rects, bool *placed, size_t count)
{
    if (!count) {
        return;
    }
    struct arrangement layout = { rects, VELA_NEW_N(size_t, count), placed, count };
    find_groups(&layout);
    size_t remaining = count - place_group(&layout, 0, rects[0]);
    while (remaining > 0) {
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
                    if (!closest_on_edge(&layout, i, &candidate, b, k < 2)) {
                        continue;
                    }
                    int64_t distance = llabs(candidate.x - a.x) + llabs(candidate.y - a.y);
                    if (distance < best_distance) {
                        best_distance = distance;
                        best_index = i;
                        best = candidate;
                    }
                }
            }
        }
        // The leftmost member of a group always fits against the rightmost placed edge.
        remaining -= place_group(&layout, best_index, best);
    }
    free(layout.groups);
}
