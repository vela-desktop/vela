// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11_space.h"

#include <float.h>
#include <math.h>
#include <string.h>

// A product that is an integer up to floating point error stays that integer.
#define EPSILON 1e-6

// The pixels per logical unit of an output: its scale, unless the rounding of
// the logical size leaves a little more.
static double density(const struct vela_x11_output *o)
{
    double d = o->scale > 0.0 ? o->scale : 1.0;
    if (o->width > 0 && o->pixel_width > d * o->width) {
        d = (double)o->pixel_width / o->width;
    }
    if (o->height > 0 && o->pixel_height > d * o->height) {
        d = (double)o->pixel_height / o->height;
    }
    return d;
}

void vela_x11_place(struct vela_x11_output *outputs, size_t count)
{
    if (count == 0) {
        return;
    }
    int left = outputs[0].x;
    int top = outputs[0].y;
    double most = 0.0;
    for (size_t i = 0; i < count; ++i) {
        left = outputs[i].x < left ? outputs[i].x : left;
        top = outputs[i].y < top ? outputs[i].y : top;
        double d = density(&outputs[i]);
        most = d > most ? d : most;
    }
    // An output ends at most at (x + width) * most in the root, where the next
    // one begins: rounding the origins up keeps that true in integers.
    for (size_t i = 0; i < count; ++i) {
        outputs[i].root_x = (int)ceil((outputs[i].x - left) * most - EPSILON);
        outputs[i].root_y = (int)ceil((outputs[i].y - top) * most - EPSILON);
    }
}

static double distance(double v, int start, int size)
{
    return v < start ? start - v : v >= start + size ? v - (start + size) + 1.0 : 0.0;
}

const struct vela_x11_output *vela_x11_output_at(const struct vela_x11_output *outputs, size_t count, double x,
    double y)
{
    const struct vela_x11_output *nearest = NULL;
    double best = DBL_MAX;
    for (size_t i = 0; i < count; ++i) {
        const struct vela_x11_output *o = &outputs[i];
        double d = hypot(distance(x, o->root_x, o->pixel_width), distance(y, o->root_y, o->pixel_height));
        if (d < best) {
            best = d;
            nearest = o;
        }
    }
    return nearest;
}

void vela_x11_from_logical(const struct vela_x11_output *output, double lx, double ly, double *x, double *y)
{
    *x = output->root_x + (lx - output->x) * output->scale;
    *y = output->root_y + (ly - output->y) * output->scale;
}

void vela_x11_to_logical(const struct vela_x11_output *output, double x, double y, double *lx, double *ly)
{
    *lx = output->x + (x - output->root_x) / output->scale;
    *ly = output->y + (y - output->root_y) / output->scale;
}

int vela_x11_dpi(const struct vela_x11_output *outputs, size_t count)
{
    double most = 1.0;
    for (size_t i = 0; i < count; ++i) {
        most = outputs[i].scale > most ? outputs[i].scale : most;
    }
    return (int)lround(96.0 * most);
}

// One resource per line: "Xft.dpi:" with spaces or tabs allowed around the
// name, as Xlib and the toolkits read it.
bool vela_x11_resources_set_dpi(const char *resources, size_t length)
{
    static const char name[] = "Xft.dpi";
    size_t i = 0;
    while (i < length) {
        while (i < length && (resources[i] == ' ' || resources[i] == '\t')) {
            ++i;
        }
        bool named = length - i >= sizeof(name) - 1 && memcmp(resources + i, name, sizeof(name) - 1) == 0;
        if (named) {
            i += sizeof(name) - 1;
            while (i < length && (resources[i] == ' ' || resources[i] == '\t')) {
                ++i;
            }
            if (i < length && resources[i] == ':') {
                return true;
            }
        }
        while (i < length && resources[i] != '\n') {
            ++i;
        }
        ++i; // past the newline
    }
    return false;
}
