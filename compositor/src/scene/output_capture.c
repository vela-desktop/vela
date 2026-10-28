// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2023 The wlroots contributors
// SPDX-License-Identifier: MIT
// Output/cursor source adapter derived from wlroots 0.20 output.c. Keep this
// protocol-facing lifecycle separate from the renderer's readback queue.
/*
Copyright (c) 2017, 2018 Drew DeVault
Copyright (c) 2014 Jari Vetoniemi
Copyright (c) 2023 The wlroots contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>
#include "ext-image-capture-source-v1-protocol.h"

#include "scene/capture.h"
#include "server.h"

struct vela_output_capture_manager {
    struct wl_global *global;
    struct vela_server *server;
    struct wl_listener display_destroy;
};


struct output_cursor_source {
	struct wlr_ext_image_capture_source_v1_cursor base;

	struct wlr_output *output;
	struct wlr_buffer *prev_buffer;
	bool initialized;
	bool needs_frame;

	struct wl_listener output_commit;
	struct wl_listener prev_buffer_release;
};

struct vela_output_capture_source {
	struct wlr_ext_image_capture_source_v1 base;
	struct wlr_addon addon;
	struct vela_server *server;

	struct wlr_output *output;

	struct wl_listener output_commit;

	struct output_cursor_source cursor;

	size_t num_started;
	bool software_cursors_locked;
};

struct vela_output_capture_source_frame_event {
	struct wlr_ext_image_capture_source_v1_frame_event base;
	struct wlr_buffer *buffer;
	struct timespec when;
};

static void output_source_start(struct wlr_ext_image_capture_source_v1 *base,
		bool with_cursors) {
	struct vela_output_capture_source *source = wl_container_of(base, source, base);
	source->num_started++;
	if (source->num_started > 1) {
		return;
	}
	wlr_output_lock_attach_render(source->output, true);
	if (with_cursors) {
		wlr_output_lock_software_cursors(source->output, true);
	}
	source->software_cursors_locked = with_cursors;
}

static void output_source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	struct vela_output_capture_source *source = wl_container_of(base, source, base);
	assert(source->num_started > 0);
	source->num_started--;
	if (source->num_started > 0) {
		return;
	}
	wlr_output_lock_attach_render(source->output, false);
	if (source->software_cursors_locked) {
		wlr_output_lock_software_cursors(source->output, false);
	}
}

static void output_source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	struct vela_output_capture_source *source = wl_container_of(base, source, base);
	// Always request the next output buffer, including after a deferred ready
	// cleared wlroots' accumulated damage. Each offered buffer is full damage,
	// so commits arriving during an in-flight copy cannot lose a later change.
	wlr_output_update_needs_frame(source->output);
}

static void output_source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *base_event) {
	struct vela_output_capture_source *source = wl_container_of(base, source, base);
	struct vela_output_capture_source_frame_event *event =
		wl_container_of(base_event, event, base);

	vela_capture_copy_frame(base, frame, event->buffer, source->server->renderer,
		source->server->loop, source->output->transform, &event->when);
}

static struct wlr_ext_image_capture_source_v1_cursor *output_source_get_pointer_cursor(
		struct wlr_ext_image_capture_source_v1 *base, struct wlr_seat *seat) {
	// TODO: handle seat
	struct vela_output_capture_source *source = wl_container_of(base, source, base);
	return &source->cursor.base;
}

static const struct wlr_ext_image_capture_source_v1_interface output_source_impl = {
	.start = output_source_start,
	.stop = output_source_stop,
	.request_frame = output_source_request_frame,
	.copy_frame = output_source_copy_frame,
	.get_pointer_cursor = output_source_get_pointer_cursor,
};

static void source_update_buffer_constraints(struct vela_output_capture_source *source) {
	struct wlr_output *output = source->output;

	if (!output->enabled) {
		wl_signal_emit_mutable(&source->base.events.constraints_update, NULL);
		return;
	}

	if (!wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain)) {
		return;
	}

	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&source->base,
		output->swapchain, output->renderer);
}

static void source_handle_output_commit(struct wl_listener *listener,
		void *data) {
	struct vela_output_capture_source *source = wl_container_of(listener, source, output_commit);
	struct wlr_output_event_commit *event = data;

	if (event->state->committed & (WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_RENDER_FORMAT | WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_TRANSFORM)) {
		source_update_buffer_constraints(source);
	}

	if (event->state->committed & WLR_OUTPUT_STATE_BUFFER) {
		struct wlr_buffer *buffer = event->state->buffer;

		pixman_region32_t full_damage;
		pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);

		struct vela_output_capture_source_frame_event frame_event = {
			.base = {
				.damage = &full_damage,
			},
			.buffer = buffer,
			.when = event->when, // TODO: predict next presentation time instead
		};
		wl_signal_emit_mutable(&source->base.events.frame, &frame_event);

		pixman_region32_fini(&full_damage);
	}
}

static void output_cursor_source_init(struct output_cursor_source *cursor_source,
	struct wlr_output *output);
static void output_cursor_source_finish(struct output_cursor_source *cursor_source);

static void output_addon_destroy(struct wlr_addon *addon) {
	struct vela_output_capture_source *source = wl_container_of(addon, source, addon);
	wlr_ext_image_capture_source_v1_finish(&source->base);
	output_cursor_source_finish(&source->cursor);
	wl_list_remove(&source->output_commit.link);
	wlr_addon_finish(&source->addon);
	free(source);
}

static const struct wlr_addon_interface output_addon_impl = {
	.name = "wlr_ext_output_image_capture_source_v1",
	.destroy = output_addon_destroy,
};

static void output_manager_handle_create_source(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t new_id,
		struct wl_resource *output_resource) {
	struct vela_output_capture_manager *manager = wl_resource_get_user_data(manager_resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);
	if (output == NULL) {
		wlr_ext_image_capture_source_v1_create_resource(NULL, client, new_id);
		return;
	}

	struct vela_output_capture_source *source;
	struct wlr_addon *addon = wlr_addon_find(&output->addons, NULL, &output_addon_impl);
	if (addon != NULL) {
		source = wl_container_of(addon, source, addon);
	} else {
		source = calloc(1, sizeof(*source));
		if (source == NULL) {
			wl_resource_post_no_memory(manager_resource);
			return;
		}

		wlr_ext_image_capture_source_v1_init(&source->base, &output_source_impl);
		wlr_addon_init(&source->addon, &output->addons, NULL, &output_addon_impl);
		source->output = output;
		source->server = manager->server;

		source->output_commit.notify = source_handle_output_commit;
		wl_signal_add(&output->events.commit, &source->output_commit);

		source_update_buffer_constraints(source);

		output_cursor_source_init(&source->cursor, output);
	}

	if (!wlr_ext_image_capture_source_v1_create_resource(&source->base, client, new_id)) {
		return;
	}
}

static void output_manager_handle_destroy(struct wl_client *client,
		struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface output_manager_impl = {
	.create_source = output_manager_handle_create_source,
	.destroy = output_manager_handle_destroy,
};

static void output_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct vela_output_capture_manager *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&ext_output_image_capture_source_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &output_manager_impl, manager, NULL);
}

static void output_manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct vela_output_capture_manager *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

bool vela_output_capture_manager_create(struct vela_server *server)
{
    struct vela_output_capture_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) return false;
    manager->server = server;
    manager->global = wl_global_create(server->display,
        &ext_output_image_capture_source_manager_v1_interface, 1, manager, output_manager_bind);
    if (!manager->global) {
        free(manager);
        return false;
    }
    manager->display_destroy.notify = output_manager_handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &manager->display_destroy);
    return true;
}

static void output_cursor_source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	struct output_cursor_source *cursor_source = wl_container_of(base, cursor_source, base);
	if (schedule_frame) {
		wlr_output_update_needs_frame(cursor_source->output);
		cursor_source->needs_frame = true;
	}
}

static void output_cursor_source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *base_event) {
	struct output_cursor_source *cursor_source = wl_container_of(base, cursor_source, base);

	struct wlr_buffer *src_buffer = cursor_source->output->cursor_front_buffer;
	if (src_buffer == NULL) {
		wlr_ext_image_copy_capture_frame_v1_fail(frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
		return;
	}

	if (!wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame,
			src_buffer, cursor_source->output->renderer)) {
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	wlr_ext_image_copy_capture_frame_v1_ready(frame,
			cursor_source->output->transform, &now);
}

static const struct wlr_ext_image_capture_source_v1_interface output_cursor_source_impl = {
	.request_frame = output_cursor_source_request_frame,
	.copy_frame = output_cursor_source_copy_frame,
};

static void output_cursor_source_update(struct output_cursor_source *cursor_source) {
	struct wlr_output *output = cursor_source->output;

	if (output->cursor_swapchain != NULL && !cursor_source->initialized) {
		wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&cursor_source->base.base,
			output->cursor_swapchain, output->renderer);
		cursor_source->initialized = true;
	}

	struct wlr_output_cursor *output_cursor = output->hardware_cursor;
	if (output_cursor == NULL || !output_cursor->visible) {
		cursor_source->base.entered = false;
		wl_signal_emit_mutable(&cursor_source->base.events.update, NULL);
		return;
	}

	if (output->cursor_swapchain != NULL &&
			((int)cursor_source->base.base.width != output->cursor_swapchain->width ||
			(int)cursor_source->base.base.height != output->cursor_swapchain->height)) {
		cursor_source->base.base.width = output->cursor_swapchain->width;
		cursor_source->base.base.height = output->cursor_swapchain->height;
		wl_signal_emit_mutable(&cursor_source->base.base.events.constraints_update, NULL);
	}

	cursor_source->base.entered = true;
	cursor_source->base.x = round(output_cursor->x);
	cursor_source->base.y = round(output_cursor->y);
	cursor_source->base.hotspot.x = output_cursor->hotspot_x;
	cursor_source->base.hotspot.y = output_cursor->hotspot_y;
	wl_signal_emit_mutable(&cursor_source->base.events.update, NULL);
}

static void output_cursor_source_handle_prev_buffer_release(struct wl_listener *listener,
		void *data) {
	struct output_cursor_source *cursor_source = wl_container_of(listener, cursor_source, prev_buffer_release);
	wl_list_remove(&cursor_source->prev_buffer_release.link);
	wl_list_init(&cursor_source->prev_buffer_release.link);
	cursor_source->prev_buffer = NULL;
}

static void output_cursor_source_handle_output_commit(struct wl_listener *listener,
		void *data) {
	struct output_cursor_source *cursor_source = wl_container_of(listener, cursor_source, output_commit);
	struct wlr_output_event_commit *event = data;

	output_cursor_source_update(cursor_source);

	struct wlr_buffer *buffer = cursor_source->output->cursor_front_buffer;
	if (buffer != NULL && (buffer != cursor_source->prev_buffer || cursor_source->needs_frame)) {
		pixman_region32_t full_damage;
		pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);
		struct vela_output_capture_source_frame_event frame_event = {
			.base = {
				.damage = &full_damage,
			},
			.buffer = buffer,
			.when = event->when, // TODO: predict next presentation time instead
		};
		wl_signal_emit_mutable(&cursor_source->base.base.events.frame, &frame_event);

		pixman_region32_fini(&full_damage);

		assert(buffer->n_locks > 0);
		cursor_source->prev_buffer = buffer;
		wl_list_remove(&cursor_source->prev_buffer_release.link);
		cursor_source->prev_buffer_release.notify = output_cursor_source_handle_prev_buffer_release;
		wl_signal_add(&buffer->events.release, &cursor_source->prev_buffer_release);
	}

	cursor_source->needs_frame = false;
}

static void output_cursor_source_init(struct output_cursor_source *cursor_source,
		struct wlr_output *output) {
	wlr_ext_image_capture_source_v1_cursor_init(&cursor_source->base, &output_cursor_source_impl);

	// Caller is responsible for destroying the output cursor source when the
	// output is destroyed
	cursor_source->output = output;

	cursor_source->output_commit.notify = output_cursor_source_handle_output_commit;
	wl_signal_add(&output->events.commit, &cursor_source->output_commit);

	wl_list_init(&cursor_source->prev_buffer_release.link);

	output_cursor_source_update(cursor_source);
}

static void output_cursor_source_finish(struct output_cursor_source *cursor_source) {
	wlr_ext_image_capture_source_v1_cursor_finish(&cursor_source->base);
	wl_list_remove(&cursor_source->output_commit.link);
	wl_list_remove(&cursor_source->prev_buffer_release.link);
}
