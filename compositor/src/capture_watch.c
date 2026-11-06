// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "capture_watch.h"

#include "buffer.h"
#include "command.h"
#include "output.h"
#include "scene/capture.h"
#include "server.h"
#include "util.h"
#include "view.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

// §3: a session counts once open this long; screencopy frames of one output
// count when they come this close together, and go on counting while one is
// pending (waiting for damage on a still screen).
#define SESSION_COUNTS_NS (1 * VELA_NS_PER_SEC)
#define SCREENCOPY_GAP_NS (2 * VELA_NS_PER_SEC)
// wlroots says nothing when a screencopy frame is done: while one is pending,
// look again this often.
#define PENDING_RECHECK_NS (1 * VELA_NS_PER_SEC)

#define PORTAL_BINARY "xdg-desktop-portal-wlr"

// A Wayland client, from its connection on: its screencopy frames can only be
// seen being created (resource_created).
struct capturer {
    struct wl_list link; // vela_capture_watch.capturers, in order of connection
    struct vela_capture_watch *watch;
    struct wl_client *client;
    pid_t pid;
    bool identified; // binary and shell, read at its first capture
    bool shell;
    char binary[NAME_MAX + 1];
    struct wl_list sessions; // struct session
    struct wl_list streams; // struct screencopy_stream
    struct wl_listener resource_created;
    struct wl_listener destroy;
};

// An ext-image-copy-capture session.
struct session {
    struct wl_list link; // capturer.sessions
    struct capturer *capturer;
    struct wlr_ext_image_copy_capture_session_v1 *wlr;
    int64_t opened_ns;
    bool counts;
    struct wl_listener destroy;
};

// A client's screencopy frames of one output.
struct screencopy_stream {
    struct wl_list link; // capturer.streams
    struct capturer *capturer;
    char output[64];
    int64_t last_ns; // the newest frame
    bool counts;
    struct wl_list frames; // struct screencopy_frame
};

struct screencopy_frame {
    struct wl_list link; // vela_capture_watch.unresolved, then its stream's frames
    struct capturer *capturer;
    struct screencopy_stream *stream; // NULL until resolved
    struct wl_resource *resource;
    struct wl_listener destroy;
};

struct vela_capture_watch {
    struct vela_server *server;
    bool unreported; // VELA_CAPTURE_UNREPORTED=1
    bool finishing;
    struct wl_list capturers;
    struct wl_list unresolved; // screencopy frames whose request is still being handled
    struct wl_event_source *timer;
    struct wl_event_source *idle; // resolves `unresolved`
    char *published; // the last line sent
    struct wl_listener client_created;
    struct wl_listener new_session;
};

static void publish(struct vela_capture_watch *watch);
static void handle_client_destroy(struct wl_listener *listener, void *data);

// ------------------------------------------------------------- identity --

static void read_binary(pid_t pid, char *binary, size_t size)
{
    binary[0] = '\0';
    char link[64], target[PATH_MAX];
    snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
    ssize_t length = readlink(link, target, sizeof(target) - 1);
    if (length <= 0) {
        return;
    }
    target[length] = '\0';
    // A binary replaced while running (an update).
    char *deleted = strstr(target, " (deleted)");
    if (deleted && deleted[10] == '\0') {
        *deleted = '\0';
    }
    const char *slash = strrchr(target, '/');
    const char *name = slash ? slash + 1 : target;
    size_t length_name = strnlen(name, size - 1);
    memcpy(binary, name, length_name);
    binary[length_name] = '\0';
}

static pid_t parent_of(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *file = fopen(path, "re");
    if (!file) {
        return 0;
    }
    char text[512];
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[length] = '\0';
    // "pid (comm) state ppid ...": comm may contain anything, even ')'.
    const char *end = strrchr(text, ')');
    int parent = 0;
    if (!end || sscanf(end + 1, " %*c %d", &parent) != 1) {
        return 0;
    }
    return (pid_t)parent;
}

// §4.2: the compositor's child, or what `/bin/sh -c` started when it didn't
// exec. Apps the shell launches are detached: their parent isn't the shell.
static bool is_shell(const struct vela_server *server, const struct capturer *capturer)
{
    const pid_t shell = server->shell.pid;
    if (shell <= 0 || capturer->pid <= 0) {
        return false;
    }
    return capturer->pid == shell
        || (parent_of(capturer->pid) == shell && strcmp(capturer->binary, "vela-shell") == 0);
}

static void identify(struct capturer *capturer)
{
    if (!capturer->identified) {
        capturer->identified = true;
        read_binary(capturer->pid, capturer->binary, sizeof(capturer->binary));
        capturer->shell = is_shell(capturer->watch->server, capturer);
    }
}

// ---------------------------------------------------------------- state --

static bool frame_pending(const struct screencopy_frame *frame)
{
    // wlroots clears the frame from its resource when it is done or failed.
    return wl_resource_get_user_data(frame->resource) != NULL;
}

static void free_frame(struct screencopy_frame *frame)
{
    wl_list_remove(&frame->link);
    wl_list_remove(&frame->destroy.link);
    free(frame);
}

static void free_stream(struct screencopy_stream *stream)
{
    struct screencopy_frame *frame, *next;
    wl_list_for_each_safe (frame, next, &stream->frames, link) {
        free_frame(frame);
    }
    wl_list_remove(&stream->link);
    free(stream);
}

static void free_session(struct session *session)
{
    wl_list_remove(&session->link);
    wl_list_remove(&session->destroy.link);
    free(session);
}

static void consider(int64_t *next, int64_t at)
{
    if (*next == 0 || at < *next) {
        *next = at;
    }
}

// What counts now (§3), and when that could change next (0: never by itself).
static int64_t evaluate(struct vela_capture_watch *watch, int64_t now)
{
    int64_t next = 0;
    struct capturer *capturer;
    wl_list_for_each (capturer, &watch->capturers, link) {
        struct session *session;
        wl_list_for_each (session, &capturer->sessions, link) {
            session->counts = now - session->opened_ns >= SESSION_COUNTS_NS;
            if (!session->counts) {
                consider(&next, session->opened_ns + SESSION_COUNTS_NS);
            }
        }
        struct screencopy_stream *stream, *next_stream;
        wl_list_for_each_safe (stream, next_stream, &capturer->streams, link) {
            bool pending = false;
            struct screencopy_frame *frame;
            wl_list_for_each (frame, &stream->frames, link) {
                pending = pending || frame_pending(frame);
            }
            const bool recent = now - stream->last_ns <= SCREENCOPY_GAP_NS;
            if (stream->counts && !recent && !pending) {
                stream->counts = false;
            }
            if (pending && stream->counts) {
                consider(&next, now + PENDING_RECHECK_NS);
            } else if (recent) {
                consider(&next, stream->last_ns + SCREENCOPY_GAP_NS + 1);
            } else if (wl_list_empty(&stream->frames)) {
                free_stream(stream); // nothing left that could make it count
            }
        }
    }
    return next;
}

static void schedule(struct vela_capture_watch *watch, int64_t next, int64_t now)
{
    if (next == 0) {
        wl_event_source_timer_update(watch->timer, 0);
        return;
    }
    int64_t ms = (next - now + VELA_NS_PER_MS - 1) / VELA_NS_PER_MS;
    wl_event_source_timer_update(watch->timer, ms < 1 ? 1 : (int)ms);
}

static void update(struct vela_capture_watch *watch)
{
    if (watch->finishing) {
        return;
    }
    const int64_t now = vela_now_ns();
    schedule(watch, evaluate(watch, now), now);
    publish(watch);
}

static int handle_timer(void *data)
{
    update(data);
    return 0;
}

// ---------------------------------------------------------- ext sessions --

static void handle_session_destroy(struct wl_listener *listener, void *data)
{
    struct session *session = wl_container_of(listener, session, destroy);
    struct vela_capture_watch *watch = session->capturer->watch;
    free_session(session);
    update(watch);
}

static void handle_new_session(struct wl_listener *listener, void *data)
{
    struct vela_capture_watch *watch = wl_container_of(listener, watch, new_session);
    struct wlr_ext_image_copy_capture_session_v1 *wlr = data;
    struct wl_listener *destroy
        = wl_client_get_destroy_listener(wl_resource_get_client(wlr->resource), handle_client_destroy);
    if (!destroy) {
        return; // connected before the watch existed: none do
    }
    struct capturer *capturer = wl_container_of(destroy, capturer, destroy);
    identify(capturer);
    if (capturer->shell) {
        return;
    }
    struct session *session = calloc(1, sizeof(*session));
    if (!session) {
        return;
    }
    session->capturer = capturer;
    session->wlr = wlr;
    session->opened_ns = vela_now_ns();
    wl_list_insert(capturer->sessions.prev, &session->link);
    session->destroy.notify = handle_session_destroy;
    wl_signal_add(&wlr->events.destroy, &session->destroy);
    update(watch);
}

// ----------------------------------------------------- screencopy frames --

static void handle_frame_destroy(struct wl_listener *listener, void *data)
{
    struct screencopy_frame *frame = wl_container_of(listener, frame, destroy);
    struct vela_capture_watch *watch = frame->capturer->watch;
    free_frame(frame);
    update(watch);
}

// The request that made the frame has been handled: its output is known.
static void resolve_frames(void *data)
{
    struct vela_capture_watch *watch = data;
    watch->idle = NULL;
    const int64_t now = vela_now_ns();
    struct screencopy_frame *frame, *next;
    wl_list_for_each_safe (frame, next, &watch->unresolved, link) {
        struct wlr_screencopy_frame_v1 *wlr = wl_resource_get_user_data(frame->resource);
        if (!wlr || !wlr->output) {
            free_frame(frame); // failed at once: it will never copy anything
            continue;
        }
        struct capturer *capturer = frame->capturer;
        struct screencopy_stream *stream = NULL, *candidate;
        wl_list_for_each (candidate, &capturer->streams, link) {
            if (strcmp(candidate->output, wlr->output->name) == 0) {
                stream = candidate;
                break;
            }
        }
        if (!stream) {
            stream = calloc(1, sizeof(*stream));
            if (!stream) {
                free_frame(frame);
                continue;
            }
            stream->capturer = capturer;
            snprintf(stream->output, sizeof(stream->output), "%s", wlr->output->name);
            wl_list_init(&stream->frames);
            wl_list_insert(capturer->streams.prev, &stream->link);
        } else if (now - stream->last_ns <= SCREENCOPY_GAP_NS) {
            stream->counts = true; // the same output again, soon: not a screenshot
        }
        stream->last_ns = now;
        frame->stream = stream;
        wl_list_remove(&frame->link);
        wl_list_insert(stream->frames.prev, &frame->link);
    }
    update(watch);
}

static void handle_resource_created(struct wl_listener *listener, void *data)
{
    struct capturer *capturer = wl_container_of(listener, capturer, resource_created);
    struct wl_resource *resource = data;
    const char *class = wl_resource_get_class(resource);
    // Called for every object of every client: the first letter rules out
    // almost all of them.
    if (class[0] != 'z' || strcmp(class, "zwlr_screencopy_frame_v1") != 0) {
        return;
    }
    struct vela_capture_watch *watch = capturer->watch;
    identify(capturer);
    if (capturer->shell) {
        return;
    }
    struct screencopy_frame *frame = calloc(1, sizeof(*frame));
    if (!frame) {
        return;
    }
    frame->capturer = capturer;
    frame->resource = resource;
    wl_list_insert(watch->unresolved.prev, &frame->link);
    frame->destroy.notify = handle_frame_destroy;
    wl_resource_add_destroy_listener(resource, &frame->destroy);
    if (!watch->idle) {
        watch->idle = wl_event_loop_add_idle(watch->server->loop, resolve_frames, watch);
    }
}

// --------------------------------------------------------------- clients --

static void free_capturer(struct capturer *capturer)
{
    struct session *session, *next_session;
    wl_list_for_each_safe (session, next_session, &capturer->sessions, link) {
        free_session(session);
    }
    struct screencopy_stream *stream, *next_stream;
    wl_list_for_each_safe (stream, next_stream, &capturer->streams, link) {
        free_stream(stream);
    }
    struct screencopy_frame *frame, *next_frame;
    wl_list_for_each_safe (frame, next_frame, &capturer->watch->unresolved, link) {
        if (frame->capturer == capturer) {
            free_frame(frame);
        }
    }
    wl_list_remove(&capturer->resource_created.link);
    wl_list_remove(&capturer->destroy.link);
    wl_list_remove(&capturer->link);
    free(capturer);
}

// Before its objects are destroyed: everything this client had goes now.
static void handle_client_destroy(struct wl_listener *listener, void *data)
{
    struct capturer *capturer = wl_container_of(listener, capturer, destroy);
    struct vela_capture_watch *watch = capturer->watch;
    free_capturer(capturer);
    update(watch);
}

static void handle_client_created(struct wl_listener *listener, void *data)
{
    struct vela_capture_watch *watch = wl_container_of(listener, watch, client_created);
    struct wl_client *client = data;
    struct capturer *capturer = calloc(1, sizeof(*capturer));
    if (!capturer) {
        return;
    }
    capturer->watch = watch;
    capturer->client = client;
    wl_client_get_credentials(client, &capturer->pid, NULL, NULL);
    wl_list_init(&capturer->sessions);
    wl_list_init(&capturer->streams);
    wl_list_insert(watch->capturers.prev, &capturer->link);
    capturer->resource_created.notify = handle_resource_created;
    wl_client_add_resource_created_listener(client, &capturer->resource_created);
    capturer->destroy.notify = handle_client_destroy;
    wl_client_add_destroy_listener(client, &capturer->destroy);
}

// ------------------------------------------------------------------ json --

static struct vela_view *view_of_source(struct vela_server *server, struct wlr_ext_image_capture_source_v1 *source)
{
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->capture && vela_window_capture_has_source(view->capture, source)) {
            return view;
        }
    }
    return NULL;
}

static struct vela_output *output_named(struct vela_server *server, const char *name)
{
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        if (strcmp(output->wlr->name, name) == 0) {
            return output;
        }
    }
    return NULL;
}

// The client's first window: OBS has one, wf-recorder doesn't.
static struct vela_view *window_of(struct vela_server *server, struct wl_client *client)
{
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->xdg && view->xdg->resource && wl_resource_get_client(view->xdg->resource) == client) {
            return view;
        }
    }
    return NULL;
}

// One target, unless the client already has it: `key` names it.
struct targets {
    struct vela_buffer json;
    char keys[16][80];
    int count;
};

static bool add_target(struct targets *targets, const char *key)
{
    for (int i = 0; i < targets->count; ++i) {
        if (strcmp(targets->keys[i], key) == 0) {
            return false;
        }
    }
    if (targets->count == (int)(sizeof(targets->keys) / sizeof(targets->keys[0]))) {
        return false;
    }
    snprintf(targets->keys[targets->count], sizeof(targets->keys[0]), "%s", key);
    vela_buffer_append(&targets->json, targets->count++ ? ",{" : "{");
    return true;
}

static void output_target(struct targets *targets, struct wlr_output *output)
{
    char key[80];
    snprintf(key, sizeof(key), "o:%s", output->name);
    if (!add_target(targets, key)) {
        return;
    }
    vela_buffer_append(&targets->json, "\"type\":\"output\",\"name\":");
    vela_buffer_append_json(&targets->json, output->name);
    vela_buffer_append(&targets->json, ",\"make\":");
    vela_buffer_append_json(&targets->json, output->make);
    vela_buffer_append(&targets->json, ",\"model\":");
    vela_buffer_append_json(&targets->json, output->model);
    vela_buffer_append(&targets->json, ",\"description\":");
    vela_buffer_append_json(&targets->json, output->description);
    vela_buffer_append(&targets->json, "}");
}

static void window_target(struct targets *targets, struct vela_view *view)
{
    const char *id = view->ext_handle && view->ext_handle->identifier ? view->ext_handle->identifier : "";
    char key[80];
    snprintf(key, sizeof(key), "w:%p", (void *)view);
    if (!add_target(targets, key)) {
        return;
    }
    vela_buffer_append(&targets->json, "\"type\":\"window\",\"id\":");
    vela_buffer_append_json(&targets->json, id);
    vela_buffer_append(&targets->json, ",\"app_id\":");
    vela_buffer_append_json(&targets->json, vela_view_app_id(view));
    vela_buffer_append(&targets->json, ",\"title\":");
    vela_buffer_append_json(&targets->json, vela_view_title(view));
    vela_buffer_append(&targets->json, "}");
}

void vela_capture_json(struct vela_server *server, struct vela_buffer *json)
{
    vela_buffer_append(json, "{\"clients\":[");
    struct vela_capture_watch *watch = server->capture_watch;
    if (!watch) {
        vela_buffer_append(json, "]}");
        return;
    }
    bool first = true;
    struct capturer *capturer;
    wl_list_for_each (capturer, &watch->capturers, link) {
        struct targets targets = { 0 };
        struct session *session;
        wl_list_for_each (session, &capturer->sessions, link) {
            if (!session->counts) {
                continue;
            }
            struct wlr_ext_image_capture_source_v1 *source = session->wlr->source;
            struct wlr_output *output = vela_output_capture_source_output(source);
            struct vela_view *view = output ? NULL : view_of_source(server, source);
            if (output) {
                output_target(&targets, output);
            } else if (view) {
                window_target(&targets, view);
            }
        }
        struct screencopy_stream *stream;
        wl_list_for_each (stream, &capturer->streams, link) {
            struct vela_output *output = stream->counts ? output_named(server, stream->output) : NULL;
            if (output) {
                output_target(&targets, output->wlr);
            }
        }
        if (targets.count > 0) {
            vela_buffer_appendf(json, "%s{\"pid\":%d,\"binary\":", first ? "" : ",", (int)capturer->pid);
            vela_buffer_append_json(json, capturer->binary);
            struct vela_view *window = window_of(server, capturer->client);
            vela_buffer_append(json, ",\"app_id\":");
            vela_buffer_append_json(json, window ? vela_view_app_id(window) : "");
            vela_buffer_append(json, ",\"window\":");
            vela_buffer_append_json(json,
                window && window->ext_handle && window->ext_handle->identifier ? window->ext_handle->identifier : "");
            vela_buffer_appendf(json, ",\"portal\":%s,\"targets\":[%s]}",
                strcmp(capturer->binary, PORTAL_BINARY) == 0 ? "true" : "false", vela_buffer_text(&targets.json));
            first = false;
        }
        vela_buffer_finish(&targets.json);
    }
    vela_buffer_append(json, "]}");
}

// Watchers get the whole state again when what they would see changed.
static void publish(struct vela_capture_watch *watch)
{
    struct vela_buffer json = { 0 };
    vela_capture_json(watch->server, &json);
    const char *text = vela_buffer_text(&json);
    if (!watch->published || strcmp(watch->published, text) != 0) {
        free(watch->published);
        watch->published = strdup(text);
        vela_commands_notify(watch->server, "capture");
    }
    vela_buffer_finish(&json);
}

void vela_capture_watch_refresh(struct vela_capture_watch *watch)
{
    if (watch && !watch->finishing) {
        publish(watch);
    }
}

// ------------------------------------------------------------- lifetime --

struct vela_capture_watch *vela_capture_watch_create(struct vela_server *server,
    struct wlr_ext_image_copy_capture_manager_v1 *copy_capture, struct wlr_screencopy_manager_v1 *screencopy)
{
    (void)screencopy; // its frames are seen per client (resource_created)
    struct vela_capture_watch *watch = calloc(1, sizeof(*watch));
    if (!watch) {
        return NULL;
    }
    watch->server = server;
    wl_list_init(&watch->capturers);
    wl_list_init(&watch->unresolved);
    wl_list_init(&watch->client_created.link);
    wl_list_init(&watch->new_session.link);
    const char *unreported = getenv("VELA_CAPTURE_UNREPORTED");
    watch->unreported = unreported && strcmp(unreported, "1") == 0;
    if (watch->unreported) {
        wlr_log(WLR_INFO, "VELA_CAPTURE_UNREPORTED=1: screen captures are not reported");
        return watch;
    }
    watch->timer = wl_event_loop_add_timer(server->loop, handle_timer, watch);
    watch->client_created.notify = handle_client_created;
    wl_display_add_client_created_listener(server->display, &watch->client_created);
    if (copy_capture) {
        watch->new_session.notify = handle_new_session;
        wl_signal_add(&copy_capture->events.new_session, &watch->new_session);
    }
    return watch;
}

void vela_capture_watch_destroy(struct vela_capture_watch *watch)
{
    if (!watch) {
        return;
    }
    watch->finishing = true;
    struct capturer *capturer, *next;
    wl_list_for_each_safe (capturer, next, &watch->capturers, link) {
        free_capturer(capturer);
    }
    wl_list_remove(&watch->client_created.link);
    wl_list_remove(&watch->new_session.link);
    if (watch->idle) {
        wl_event_source_remove(watch->idle);
    }
    if (watch->timer) {
        wl_event_source_remove(watch->timer);
    }
    free(watch->published);
    free(watch);
}
