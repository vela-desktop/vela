// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the complete production serializer, with CPU metadata providers.
// Function sections discard command handlers unrelated to the state response.
#include "../src/command.c"

// ASan keeps the global topics table and its callback even with --gc-sections.
// The state response never calls this provider; fail if that contract changes.
void vela_brightness_json(struct vela_server *server, struct vela_buffer *json) { abort(); }

static struct vela_view *focused_view;
struct vela_view *vela_views_focused(struct vela_server *server) { return focused_view; }
struct wlr_box vela_view_frame_box(struct vela_view *view) { return (struct wlr_box) { 1, 2, 300, 200 }; }
struct vela_output *vela_view_output(struct vela_view *view) { return NULL; }
struct wlr_surface *vela_view_surface(const struct vela_view *view) { return NULL; }
const char *vela_view_title(const struct vela_view *view) { return "title \"quoted\""; }
const char *vela_view_app_id(const struct vela_view *view) { return "test.app"; }
bool vela_view_stays_below(const struct vela_view *view) { return false; }
struct wlr_box vela_output_box(const struct vela_output *output) { return (struct wlr_box) { 0 }; }
struct vela_output *vela_layer_surface_output(const struct vela_layer_surface *layer) { return NULL; }
void vela_node_coords(const struct vela_node *node, double *lx, double *ly) { *lx = *ly = 0; }
struct vela_readback_stats vela_renderer_readback_stats(const struct vela_renderer *renderer)
{
    return (struct vela_readback_stats) { 0 };
}
struct vela_upload_stats vela_renderer_upload_stats(const struct vela_renderer *renderer)
{
    return (struct vela_upload_stats) { .chunks_max = 16, .budget_bytes = 128u * 1024u * 1024u };
}
struct vela_upload_observation vela_texture_upload_observation(struct wlr_texture *texture)
{
    return (struct vela_upload_observation) { .frames = 2, .deferred = true, .overlapped = true };
}

void vela_test_state_json(bool has_focus, struct vela_buffer *json)
{
    struct vela_server server = { 0 };
    struct vela_workspaces workspaces = { .current = 2, .count = 4 };
    struct wlr_seat seat = { 0 };
    struct wlr_ext_foreign_toplevel_handle_v1 handle = { .identifier = "window-\"quoted\"\nidentifier" };
    struct vela_view view = { .mapped = true, .ext_handle = &handle, .workspace = 2 };
    server.locked = true;
    server.workspaces = &workspaces;
    server.seat = &seat;
    wl_list_init(&server.outputs);
    wl_list_init(&server.views);
    wl_list_init(&server.layer_surfaces);
    focused_view = has_focus ? &view : NULL;
    if (has_focus) wl_list_insert(&server.views, &view.link);
    state_json(&server, json);
}
