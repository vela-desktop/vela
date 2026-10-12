// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-only fixture: real wlroots headless outputs and Vela's actual output
// manager/preview protocol, with rendering and window rearrangement stubbed.
#include "output_manager.h"
#include "output_preview.h"
#include "output.h"
#include "output_key.h"
#include "server.h"
#include "view.h"
#include "scene/scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>

static char fail_name[64];
static int failures_left;

bool vela_output_commit_mode(struct vela_output *output, struct wlr_output_state *state)
{
    if (failures_left > 0 && strcmp(fail_name, output->wlr->name) == 0) {
        if (--failures_left == 0) fail_name[0] = '\0';
        return false;
    }
    return wlr_output_commit_state(output->wlr, state);
}
void vela_output_arrange_layers(struct vela_output *output) {}
void vela_views_check_outputs(struct vela_server *server) {}
void vela_view_keep_in_place(struct vela_view *view) {}
void vela_scene_changed(struct vela_scene *scene) {}
void vela_output_key(const struct wlr_output *output, char *out, size_t size)
{
    vela_output_key_format(out, size, output->make, output->model, output->serial, output->name);
}
struct vela_output *vela_output_named(const struct vela_server *server, const char *name)
{
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        if (!strcmp(output->wlr->name, name)) return output;
    }
    return NULL;
}
static void destroyed(struct wl_listener *listener, void *data)
{
    struct vela_output *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->link);
    wl_list_remove(&output->destroy.link);
    output->wlr->data = NULL;
    free(output);
}
static void add_output(struct vela_server *server)
{
    struct wlr_output *wlr = wlr_headless_add_output(server->backend, 1920, 1080);
    struct vela_output *output = calloc(1, sizeof(*output));
    output->server = server;
    output->wlr = wlr;
    wlr->data = output;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, 1920, 1080, 60000);
    if (!wlr_output_commit_state(wlr, &state)) abort();
    wlr_output_state_finish(&state);
    wl_list_insert(server->outputs.prev, &output->link);
    output->destroy.notify = destroyed;
    wl_signal_add(&wlr->events.destroy, &output->destroy);
    wlr_output_layout_add_auto(server->output_layout, wlr);
    vela_output_manager_update(server);
}
static void layout_changed(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, layout_change);
    vela_output_manager_update(server);
}
static int command(int fd, uint32_t mask, void *data)
{
    struct vela_server *server = data;
    char line[128] = {0};
    ssize_t n = read(fd, line, sizeof(line) - 1);
    if (n <= 0) { wl_display_terminate(server->display); return 0; }
    line[strcspn(line, "\n")] = '\0';
    if (!strcmp(line, "add")) add_output(server);
    else if (!strncmp(line, "remove ", 7)) {
        struct vela_output *output = vela_output_named(server, line + 7);
        if (output) wlr_output_destroy(output->wlr);
        vela_output_manager_update(server);
    } else if (!strncmp(line, "fail-twice ", 11)) {
        snprintf(fail_name, sizeof(fail_name), "%s", line + 11);
        failures_left = 2;
    } else if (!strncmp(line, "fail ", 5)) {
        snprintf(fail_name, sizeof(fail_name), "%s", line + 5);
        failures_left = 1;
    } else if (!strcmp(line, "quit")) wl_display_terminate(server->display);
    puts("ok"); fflush(stdout);
    return 0;
}
int main(int argc, char **argv)
{
    struct vela_server server = {0};
    server.display = wl_display_create();
    server.loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.loop);
    server.output_layout = wlr_output_layout_create(server.display);
    // Persistence is tested without creating a real DRM session.
    server.session = (void *)1;
    wl_list_init(&server.outputs);
    wl_list_init(&server.views);
    vela_output_manager_init(&server);
    if (argc > 1 && !strcmp(argv[1], "legacy")) vela_output_preview_finish(&server);
    server.layout_change.notify = layout_changed;
    wl_signal_add(&server.output_layout->events.change, &server.layout_change);
    if (!wlr_backend_start(server.backend)) return 1;
    add_output(&server);
    add_output(&server);
    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) return 1;
    struct wl_event_source *commands = wl_event_loop_add_fd(server.loop, STDIN_FILENO, WL_EVENT_READABLE, command, &server);
    puts(socket); fflush(stdout);
    wl_display_run(server.display);
    wl_display_destroy_clients(server.display);
    vela_output_manager_finish(&server);
    wl_list_remove(&server.layout_change.link);
    wl_event_source_remove(commands);
    wlr_backend_destroy(server.backend);
    wlr_output_layout_destroy(server.output_layout);
    wl_display_destroy(server.display);
    return 0;
}
