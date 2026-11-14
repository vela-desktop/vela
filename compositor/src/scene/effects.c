// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scene/effects.h"

#include "alloc/alloc.h"
#include "listen.h"
#include "scene/scene.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>

#include "ext-background-effect-v1-protocol.h"

// A surface's effect: pending region (from the client) and current one
// (applied on commit). Found from the surface through an addon. Lives as long
// as the client's resource; if the surface dies first, it stays inert.
struct effect {
    struct wl_resource *resource;
    struct wlr_surface *surface; // NULL once the surface is gone
    struct vela_scene *scene;
    pixman_region32_t pending;
    pixman_region32_t current;
    bool dirty;
    struct wlr_addon addon;
    struct wl_listener commit;
};

static void detach_surface(struct effect *effect)
{
    if (!effect->surface) {
        return;
    }
    wl_list_remove(&effect->commit.link);
    wlr_addon_finish(&effect->addon);
    effect->surface = NULL;
}

// The surface goes away: the object stays, inert.
static void handle_surface_gone(struct wlr_addon *addon)
{
    struct effect *effect = wl_container_of(addon, effect, addon);
    detach_surface(effect);
}

static const struct wlr_addon_interface effect_addon = {
    .name = "vela-background-effect",
    .destroy = handle_surface_gone,
};

// Double-buffered state: the new region applies from the commit.
static void handle_commit(struct wl_listener *listener, void *data)
{
    struct effect *effect = wl_container_of(listener, effect, commit);
    if (effect->dirty) {
        pixman_region32_copy(&effect->current, &effect->pending);
        effect->dirty = false;
        vela_scene_changed(effect->scene);
    }
}

static void handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void handle_set_blur_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region)
{
    struct effect *effect = wl_resource_get_user_data(resource);
    if (!effect->surface) {
        wl_resource_post_error(resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
            "the surface is gone");
        return;
    }
    if (region) {
        pixman_region32_copy(&effect->pending, wlr_region_from_resource(region));
    } else {
        pixman_region32_clear(&effect->pending);
    }
    effect->dirty = true;
}

static const struct ext_background_effect_surface_v1_interface surface_impl = {
    .destroy = handle_destroy,
    .set_blur_region = handle_set_blur_region,
};

static void handle_resource_destroy(struct wl_resource *resource)
{
    struct effect *effect = wl_resource_get_user_data(resource);
    bool shown = effect->surface && pixman_region32_not_empty(&effect->current);
    struct vela_scene *scene = effect->scene;
    detach_surface(effect);
    pixman_region32_fini(&effect->pending);
    pixman_region32_fini(&effect->current);
    free(effect);
    if (shown) {
        vela_scene_changed(scene);
    }
}

static void handle_get_effect(struct wl_client *client, struct wl_resource *manager, uint32_t id,
    struct wl_resource *surface_resource)
{
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
    if (wlr_addon_find(&surface->addons, NULL, &effect_addon)) {
        wl_resource_post_error(manager, EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
            "the surface already has an effect");
        return;
    }
    struct wl_resource *resource = VELA_MUST(wl_resource_create(client, &ext_background_effect_surface_v1_interface,
        wl_resource_get_version(manager), id));
    struct effect *effect = VELA_NEW(struct effect);
    effect->resource = resource;
    effect->surface = surface;
    effect->scene = wl_resource_get_user_data(manager);
    pixman_region32_init(&effect->pending);
    pixman_region32_init(&effect->current);
    wlr_addon_init(&effect->addon, &surface->addons, NULL, &effect_addon);
    vela_listen(&surface->events.commit, &effect->commit, handle_commit);
    wl_resource_set_implementation(resource, &surface_impl, effect, handle_resource_destroy);
}

static void handle_manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct ext_background_effect_manager_v1_interface manager_impl = {
    .destroy = handle_manager_destroy,
    .get_background_effect = handle_get_effect,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = VELA_MUST(wl_resource_create(client, &ext_background_effect_manager_v1_interface,
        (int)version, id));
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
    ext_background_effect_manager_v1_send_capabilities(resource, EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
}

void vela_background_effects_init(struct wl_display *display, struct vela_scene *scene)
{
    VELA_MUST(wl_global_create(display, &ext_background_effect_manager_v1_interface, 1, scene, bind_manager));
}

const pixman_region32_t *vela_blur_region(struct wlr_surface *surface)
{
    struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL, &effect_addon);
    if (!addon) {
        return NULL;
    }
    struct effect *effect = wl_container_of(addon, effect, addon);
    return pixman_region32_not_empty(&effect->current) ? &effect->current : NULL;
}
