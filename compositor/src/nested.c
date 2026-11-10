// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nested.h"

#include "alloc/alloc.h"
#include "output.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "fractional-scale-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

struct vela_nested {
    struct vela_output *output;
    struct wl_display *remote;
    struct wl_surface *surface;
    struct wl_registry *registry;
    struct wp_viewporter *viewporter;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct wp_fractional_scale_v1 *fractional;
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *inhibit_manager;
    struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    struct wl_seat *seat;

    int width; // logical size of the host window
    int height;
    double host_scale; // the host's scale (KDE at 125%: 1.25)
    double extra_scale; // VELA_SCALE, on top of the host's
};

// A buffer as large as the host window's physical pixels; Vela's output scale
// is the host's (times VELA_SCALE): so apps draw at full resolution and
// nothing is magnified twice.
static void apply(struct vela_nested *nested)
{
    int buffer_width = (int)lround(nested->width * nested->host_scale);
    int buffer_height = (int)lround(nested->height * nested->host_scale);
    float scale = (float)(nested->host_scale * nested->extra_scale);
    struct wlr_output *output = nested->output->wlr;
    if (buffer_width == output->width && buffer_height == output->height && scale == output->scale) {
        return;
    }
    if (nested->viewport) {
        wp_viewport_set_destination(nested->viewport, nested->width, nested->height);
    }
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, buffer_width, buffer_height, 0);
    wlr_output_state_set_scale(&state, scale);
    vela_output_commit_mode(nested->output, &state);
    wlr_output_state_finish(&state);
    wl_display_flush(nested->remote);
}

static void handle_preferred_scale(void *data, struct wp_fractional_scale_v1 *fractional, uint32_t scale120)
{
    struct vela_nested *nested = data;
    double scale = scale120 / 120.0;
    if (scale == nested->host_scale) {
        return;
    }
    nested->host_scale = scale;
    wlr_log(WLR_INFO, "%s: the host window has scale %.3f", nested->output->wlr->name, scale);
    apply(nested);
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = handle_preferred_scale,
};

// As the host's protocols arrive.
static void setup(struct vela_nested *nested)
{
    if (nested->viewporter && nested->fractional_manager && !nested->fractional) {
        nested->viewport = wp_viewporter_get_viewport(nested->viewporter, nested->surface);
        nested->fractional
            = wp_fractional_scale_manager_v1_get_fractional_scale(nested->fractional_manager, nested->surface);
        wp_fractional_scale_v1_add_listener(nested->fractional, &fractional_listener, nested);
    }
    if (nested->inhibit_manager && nested->seat && !nested->inhibitor) {
        nested->inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(nested->inhibit_manager,
            nested->surface, nested->seat);
        wlr_log(WLR_INFO, "%s: asked the host for the keyboard shortcuts", nested->output->wlr->name);
    }
    wl_display_flush(nested->remote);
}

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
    uint32_t version)
{
    struct vela_nested *nested = data;
    if (strcmp(interface, wp_viewporter_interface.name) == 0 && !nested->viewporter) {
        nested->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0 && !nested->fractional_manager) {
        nested->fractional_manager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0
        && !nested->inhibit_manager) {
        nested->inhibit_manager
            = wl_registry_bind(registry, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !nested->seat) {
        nested->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else {
        return;
    }
    setup(nested);
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

struct vela_nested *vela_nested_create(struct vela_output *output)
{
    struct vela_nested *nested = VELA_NEW(struct vela_nested);
    nested->output = output;
    nested->remote = wlr_wl_backend_get_remote_display(output->wlr->backend);
    nested->surface = wlr_wl_output_get_surface(output->wlr);
    nested->width = output->wlr->width;
    nested->height = output->wlr->height;
    nested->host_scale = 1.0;
    nested->extra_scale = 1.0;
    const char *scale = getenv("VELA_SCALE");
    if (scale && *scale && !strchr(scale, '=')) {
        nested->extra_scale = fmax(0.25, strtod(scale, NULL));
    }
    // The proxies are on the default queue: the wlroots backend dispatches
    // their events with its own.
    nested->registry = wl_display_get_registry(nested->remote);
    wl_registry_add_listener(nested->registry, &registry_listener, nested);
    wl_display_flush(nested->remote);
    return nested;
}

void vela_nested_destroy(struct vela_nested *nested)
{
    if (!nested) {
        return;
    }
    if (nested->inhibitor) {
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(nested->inhibitor);
    }
    if (nested->fractional) {
        wp_fractional_scale_v1_destroy(nested->fractional);
    }
    if (nested->viewport) {
        wp_viewport_destroy(nested->viewport);
    }
    if (nested->inhibit_manager) {
        zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(nested->inhibit_manager);
    }
    if (nested->fractional_manager) {
        wp_fractional_scale_manager_v1_destroy(nested->fractional_manager);
    }
    if (nested->viewporter) {
        wp_viewporter_destroy(nested->viewporter);
    }
    if (nested->seat) {
        wl_seat_destroy(nested->seat);
    }
    wl_registry_destroy(nested->registry);
    wl_display_flush(nested->remote);
    free(nested);
}

void vela_nested_resize(struct vela_nested *nested, int width, int height)
{
    // When the host lets us choose, the backend proposes the current buffer
    // size again: that isn't a logical size. KWin sends a configure on every
    // state change (activation, focus): if the size doesn't change nothing is
    // touched, or the output mode would be redone every time.
    bool buffer_size = width == nested->output->wlr->width && height == nested->output->wlr->height;
    if (width <= 0 || height <= 0 || buffer_size || (width == nested->width && height == nested->height)) {
        return;
    }
    nested->width = width;
    nested->height = height;
    apply(nested);
}

double vela_nested_pointer_scale_x(const struct vela_nested *nested)
{
    return nested->width > 0 ? (double)nested->output->wlr->width / nested->width : 1.0;
}

double vela_nested_pointer_scale_y(const struct vela_nested *nested)
{
    return nested->height > 0 ? (double)nested->output->wlr->height / nested->height : 1.0;
}
