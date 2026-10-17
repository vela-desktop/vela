// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "capture_watch.h"

#include "config.h"
#include "listen.h"
#include "server.h"
#include "shell.h"
#include "util.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <string.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>

#define TICK_MS 500
#define SESSION_MIN_AGE_NS (VELA_NS_PER_SEC) // shorter ones are screenshots and previews
#define STREAK_ON 2 // checks in a row with a screencopy frame pending
#define STREAK_OFF 2 // checks in a row without, to say it ended
#define RESEND_TICKS 10 // "capture on" again now and then: the shell may have restarted

struct session {
    struct wl_list link;
    struct vela_capture_watch *watch;
    struct wl_listener destroy;
    int64_t created_ns;
};

struct vela_capture_watch {
    struct vela_server *server;
    struct wl_event_source *timer;
    struct wl_list sessions; // struct session
    struct wl_listener new_session;
    bool enabled;
    bool dnd; // capture-dnd: "capture on dnd"
    bool on; // what the shell was told
    int seen, missed; // screencopy checks with / without a pending frame
    int ticks_on;
};

static void handle_session_destroy(struct wl_listener *listener, void *data)
{
    struct session *session = wl_container_of(listener, session, destroy);
    wl_list_remove(&session->link);
    wl_list_remove(&session->destroy.link);
    free(session);
}

static void handle_new_session(struct wl_listener *listener, void *data)
{
    struct vela_capture_watch *watch = wl_container_of(listener, watch, new_session);
    struct wlr_ext_image_copy_capture_session_v1 *wlr = data;
    struct session *session = calloc(1, sizeof(*session));
    if (!session) {
        return;
    }
    session->watch = watch;
    session->created_ns = vela_now_ns();
    wl_list_insert(&watch->sessions, &session->link);
    vela_listen(&wlr->events.destroy, &session->destroy, handle_session_destroy);
}

static void announce(struct vela_capture_watch *watch, bool on)
{
    watch->on = on;
    watch->ticks_on = 0;
    vela_shell_send(watch->server, !on ? "capture off" : watch->dnd ? "capture on dnd" : "capture on");
}

// wlroots keeps no list we can read of the screencopy frames being captured, so
// they are looked for among the clients' objects.
static enum wl_iterator_result find_frame(struct wl_resource *resource, void *data)
{
    if (strcmp(wl_resource_get_class(resource), "zwlr_screencopy_frame_v1") == 0) {
        *(bool *)data = true;
        return WL_ITERATOR_STOP;
    }
    return WL_ITERATOR_CONTINUE;
}

static bool screencopy_pending(struct vela_capture_watch *watch)
{
    bool found = false;
    struct wl_client *client;
    struct wl_list *clients = wl_display_get_client_list(watch->server->display);
    for (struct wl_list *link = clients->next; link != clients && !found; link = link->next) {
        client = wl_client_from_link(link);
        wl_client_for_each_resource(client, find_frame, &found);
    }
    return found;
}

static int handle_tick(void *data)
{
    struct vela_capture_watch *watch = data;
    wl_event_source_timer_update(watch->timer, TICK_MS);

    int64_t now = vela_now_ns();
    bool streaming = false;
    struct session *session;
    wl_list_for_each (session, &watch->sessions, link) {
        if (now - session->created_ns >= SESSION_MIN_AGE_NS) {
            streaming = true;
            break;
        }
    }
    if (screencopy_pending(watch)) {
        watch->seen = watch->seen < 1000 ? watch->seen + 1 : watch->seen;
        watch->missed = 0;
    } else if (watch->seen > 0 && ++watch->missed >= STREAK_OFF) {
        watch->seen = 0;
    }
    streaming = streaming || watch->seen >= STREAK_ON;
    streaming = streaming && watch->enabled;

    if (streaming != watch->on) {
        announce(watch, streaming);
    } else if (watch->on && ++watch->ticks_on >= RESEND_TICKS) {
        announce(watch, true);
    }
    return 0;
}

void vela_capture_watch_load(struct vela_capture_watch *watch)
{
    struct vela_config config;
    vela_config_read(&config);
    watch->enabled = vela_config_flag(&config, "capture-indicator", true);
    const bool dnd = vela_config_flag(&config, "capture-dnd", true);
    vela_config_finish(&config);
    const bool dnd_changed = dnd != watch->dnd;
    watch->dnd = dnd;
    if (!watch->enabled && watch->on) {
        announce(watch, false);
    } else if (watch->on && dnd_changed) {
        announce(watch, true); // the shell hears the new choice at once
    }
}

struct vela_capture_watch *vela_capture_watch_create(struct vela_server *server)
{
    struct vela_capture_watch *watch = calloc(1, sizeof(*watch));
    watch->server = server;
    watch->enabled = true;
    watch->dnd = true;
    wl_list_init(&watch->sessions);
    wl_list_init(&watch->new_session.link);
    watch->timer = wl_event_loop_add_timer(server->loop, handle_tick, watch);
    wl_event_source_timer_update(watch->timer, TICK_MS);
    vela_capture_watch_load(watch);
    return watch;
}

void vela_capture_watch_copy_capture(struct vela_capture_watch *watch, struct wlr_ext_image_copy_capture_manager_v1 *manager)
{
    vela_listen(&manager->events.new_session, &watch->new_session, handle_new_session);
}

void vela_capture_watch_destroy(struct vela_capture_watch *watch)
{
    if (!watch) {
        return;
    }
    wl_event_source_remove(watch->timer);
    wl_list_remove(&watch->new_session.link);
    struct session *session, *next;
    wl_list_for_each_safe (session, next, &watch->sessions, link) {
        wl_list_remove(&session->link);
        wl_list_remove(&session->destroy.link);
        free(session);
    }
    free(watch);
}
