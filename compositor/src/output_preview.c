// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output_preview.h"
#include "output_manager.h"
#include "output.h"
#include "server.h"
#include "vela-output-preview-v1-protocol.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/util/log.h>

#ifndef VELA_OUTPUT_PREVIEW_TIMEOUT_MS
#define VELA_OUTPUT_PREVIEW_TIMEOUT_MS 15000
#endif

struct vela_output_previews {
    struct vela_server *server;
    struct wl_global *global;
    struct wl_list previews;
    struct vela_output_preview *active;
};

struct vela_output_preview {
    struct vela_output_previews *owner;
    struct wl_list link;
    struct wl_resource *resource;
    struct wl_resource *configuration;
    struct wl_listener configuration_destroy;
    struct wl_event_source *timer;
    struct wlr_output_configuration_v1 *before;
    char *preserve_position;
    bool finished;
};

static void release_if_unused(struct vela_output_preview *preview)
{
    if (!preview->resource && !preview->configuration) {
        wl_list_remove(&preview->link);
        free(preview->preserve_position);
        free(preview);
    }
}

static void finish(struct vela_output_preview *preview, uint32_t result)
{
    if (preview->finished) {
        return;
    }
    preview->finished = true;
    if (preview->timer) {
        wl_event_source_remove(preview->timer);
        preview->timer = NULL;
    }
    if (preview->before) {
        if (result == VELA_OUTPUT_PREVIEW_V1_RESULT_CONFIRMED) {
            vela_output_manager_save(preview->owner->server);
        } else if (!vela_output_manager_restore(preview->owner->server, preview->before)) {
            result = VELA_OUTPUT_PREVIEW_V1_RESULT_RESTORE_FAILED;
            wlr_log(WLR_ERROR, "Could not fully restore the previous display configuration");
        }
        wlr_output_configuration_v1_destroy(preview->before);
        preview->before = NULL;
    }
    if (preview->owner->active == preview) {
        preview->owner->active = NULL;
    }
    if (preview->resource) {
        vela_output_preview_v1_send_finished(preview->resource, result);
    }
}

static int timeout(void *data)
{
    struct vela_output_preview *preview = data;
    finish(preview, VELA_OUTPUT_PREVIEW_V1_RESULT_REVERTED);
    return 0;
}

static void configuration_destroyed(struct wl_listener *listener, void *data)
{
    struct vela_output_preview *preview = wl_container_of(listener, preview, configuration_destroy);
    wl_list_remove(&preview->configuration_destroy.link);
    preview->configuration = NULL;
    finish(preview, VELA_OUTPUT_PREVIEW_V1_RESULT_FAILED);
    release_if_unused(preview);
}

static void destroy_resource(struct wl_resource *resource)
{
    struct vela_output_preview *preview = wl_resource_get_user_data(resource);
    preview->resource = NULL;
    // Runs for explicit destruction AND a dead client, even after SIGKILL.
    finish(preview, VELA_OUTPUT_PREVIEW_V1_RESULT_REVERTED);
    // Retain an abandoned configuration's marker until it is destroyed, so
    // applying it later cannot bypass the preview.
    release_if_unused(preview);
}

static void request_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void request_confirm(struct wl_client *client, struct wl_resource *resource)
{
    struct vela_output_preview *preview = wl_resource_get_user_data(resource);
    // A premature confirmation must never remove the safety net.
    if (preview->before) {
        finish(preview, VELA_OUTPUT_PREVIEW_V1_RESULT_CONFIRMED);
    }
}

static void request_revert(struct wl_client *client, struct wl_resource *resource)
{
    finish(wl_resource_get_user_data(resource), VELA_OUTPUT_PREVIEW_V1_RESULT_REVERTED);
}

static const struct vela_output_preview_v1_interface preview_impl = {
    .confirm = request_confirm,
    .revert = request_revert,
    .destroy = request_destroy,
};

static void protect(struct wl_client *client, struct wl_resource *resource, uint32_t id,
    uint32_t configuration_id, const char *preserve_position)
{
    struct wl_resource *configuration = wl_client_get_object(client, configuration_id);
    struct vela_output_previews *owner = wl_resource_get_user_data(resource);
    if (!configuration || strcmp(wl_resource_get_class(configuration), "zwlr_output_configuration_v1") != 0) {
        wl_resource_post_error(resource, VELA_OUTPUT_PREVIEW_MANAGER_V1_ERROR_INVALID_CONFIGURATION,
            "Expected a wlr output configuration owned by this client");
        return;
    }
    struct vela_output_preview *preview = calloc(1, sizeof(*preview));
    if (!preview) {
        wl_client_post_no_memory(client);
        return;
    }
    preview->owner = owner;
    preview->preserve_position = strdup(preserve_position);
    if (!preview->preserve_position) {
        free(preview);
        wl_client_post_no_memory(client);
        return;
    }
    preview->resource = wl_resource_create(client, &vela_output_preview_v1_interface, 1, id);
    if (!preview->resource) {
        free(preview->preserve_position);
        free(preview);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(preview->resource, &preview_impl, preview, destroy_resource);
    wl_list_insert(owner->previews.prev, &preview->link);
    preview->configuration = configuration;
    preview->configuration_destroy.notify = configuration_destroyed;
    wl_resource_add_destroy_listener(configuration, &preview->configuration_destroy);
    struct vela_output_preview *other;
    wl_list_for_each (other, &owner->previews, link) {
        if (other != preview && other->configuration == configuration) {
            finish(preview, VELA_OUTPUT_PREVIEW_V1_RESULT_BUSY);
            return;
        }
    }
    if (owner->active) {
        finish(preview, VELA_OUTPUT_PREVIEW_V1_RESULT_BUSY);
        return;
    }
    owner->active = preview;
    preview->timer = wl_event_loop_add_timer(owner->server->loop, timeout, preview);
    if (!preview->timer || wl_event_source_timer_update(preview->timer, VELA_OUTPUT_PREVIEW_TIMEOUT_MS) < 0) {
        finish(preview, VELA_OUTPUT_PREVIEW_V1_RESULT_FAILED);
    }
}

static const struct vela_output_preview_manager_v1_interface manager_impl = {
    .destroy = request_destroy,
    .protect = protect,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client, &vela_output_preview_manager_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

void vela_output_preview_init(struct vela_server *server)
{
    struct vela_output_previews *owner = calloc(1, sizeof(*owner));
    if (!owner) {
        return;
    }
    owner->server = server;
    wl_list_init(&owner->previews);
    owner->global = wl_global_create(server->display, &vela_output_preview_manager_v1_interface, 1, owner, bind_manager);
    if (!owner->global) {
        free(owner);
        return;
    }
    server->output_previews = owner;
}

bool vela_output_preview_can_apply(struct vela_server *server, struct wlr_output_configuration_v1 *config)
{
    struct vela_output_previews *owner = server->output_previews;
    if (!owner) {
        return true;
    }
    struct vela_output_preview *preview;
    wl_list_for_each (preview, &owner->previews, link) {
        if (config->resource && preview->configuration == config->resource && preview->finished) {
            return false;
        }
    }
    if (owner->active && owner->active->configuration != config->resource) {
        return false;
    }
    if (owner->active && owner->active->preserve_position[0]) {
        // Disabled heads do not advertise positions. A fresh client can ask
        // to retain the remembered position instead of submitting (0,0).
        struct wlr_output_configuration_head_v1 *head;
        wl_list_for_each (head, &config->heads, link) {
            struct vela_output *output = head->state.output->data;
            if (output && !output->wlr->enabled && output->has_last_position && head->state.enabled
                && !strcmp(output->wlr->name, owner->active->preserve_position)) {
                head->state.x = output->last_x;
                head->state.y = output->last_y;
            }
        }
    }
    return true;
}

bool vela_output_preview_applied(struct vela_server *server, struct wlr_output_configuration_v1 *config,
    struct wlr_output_configuration_v1 *before, bool succeeded)
{
    struct vela_output_previews *owner = server->output_previews;
    struct vela_output_preview *preview = owner ? owner->active : NULL;
    if (!preview || preview->configuration != config->resource) {
        return false;
    }
    if (!succeeded) {
        finish(preview, VELA_OUTPUT_PREVIEW_V1_RESULT_FAILED);
        return false;
    }
    // wlroots destroys the configuration after our reply; the preview now
    // owns its snapshot independently of that object's lifetime.
    wl_list_remove(&preview->configuration_destroy.link);
    preview->configuration = NULL;
    preview->before = before;
    if (wl_event_source_timer_update(preview->timer, VELA_OUTPUT_PREVIEW_TIMEOUT_MS) < 0) {
        finish(preview, VELA_OUTPUT_PREVIEW_V1_RESULT_REVERTED);
    } else {
        vela_output_preview_v1_send_applied(preview->resource, VELA_OUTPUT_PREVIEW_TIMEOUT_MS);
    }
    return true;
}

void vela_output_preview_finish(struct vela_server *server)
{
    struct vela_output_previews *owner = server->output_previews;
    if (!owner) {
        return;
    }
    struct vela_output_preview *preview, *tmp;
    wl_list_for_each_safe (preview, tmp, &owner->previews, link) {
        finish(preview, VELA_OUTPUT_PREVIEW_V1_RESULT_REVERTED);
        if (preview->configuration) {
            wl_list_remove(&preview->configuration_destroy.link);
        }
        if (preview->resource) {
            wl_resource_set_user_data(preview->resource, NULL);
            wl_resource_set_destructor(preview->resource, NULL);
        }
        wl_list_remove(&preview->link);
        free(preview->preserve_position);
        free(preview);
    }
    wl_global_destroy(owner->global);
    free(owner);
    server->output_previews = NULL;
}
