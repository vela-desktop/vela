// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Who captures the screen (capture_watch.c, docs/screen-sharing.md §3, §4),
// without a GPU: wlroots' own capture managers, a headless output drawn by
// pixman, fake capture sources, clients in this process and a clock the test
// moves. Sessions count after 1 s; screencopy frames of one output when they
// come within 2 s, and while one is pending; the shell, test-shell and
// VELA_CAPTURE_UNREPORTED report nothing; clients that go take their report.
#undef NDEBUG
#include "buffer.h"
#include "capture_watch.h"
#include "output.h"
#include "scene/capture.h"
#include "server.h"
#include "util.h"
#include "view.h"

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_event_loop *loop;
static struct vela_server server;

// ------------------------------------------------------------------ stubs --

static int64_t clock_ns = 1000 * VELA_NS_PER_SEC;

int64_t vela_now_ns(void)
{
    return clock_ns;
}

static int notified;

void vela_commands_notify(struct vela_server *s, const char *topic)
{
    assert(s == &server && strcmp(topic, "capture") == 0);
    ++notified;
}

// Two fake sources: one stands for an output's, one for a window's.
static struct wlr_ext_image_capture_source_v1 output_source, window_source;
static struct wlr_output *shared_output;

struct wlr_output *vela_output_capture_source_output(struct wlr_ext_image_capture_source_v1 *source)
{
    return source == &output_source ? shared_output : NULL;
}

bool vela_window_capture_has_source(const struct vela_window_capture *capture,
    const struct wlr_ext_image_capture_source_v1 *source)
{
    return (const void *)capture == (const void *)source;
}

static const char *window_title = "Notes";

const char *vela_view_title(const struct vela_view *view)
{
    return window_title;
}

const char *vela_view_app_id(const struct vela_view *view)
{
    return "org.vela.Notes";
}

static void source_start(struct wlr_ext_image_capture_source_v1 *source, bool with_cursors) {}
static void source_stop(struct wlr_ext_image_capture_source_v1 *source) {}
static void source_request_frame(struct wlr_ext_image_capture_source_v1 *source, bool schedule) {}
static void source_copy_frame(struct wlr_ext_image_capture_source_v1 *source,
    struct wlr_ext_image_copy_capture_frame_v1 *frame, struct wlr_ext_image_capture_source_v1_frame_event *event) {}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
    .start = source_start,
    .stop = source_stop,
    .request_frame = source_request_frame,
    .copy_frame = source_copy_frame,
};

// The foreign-toplevel capture requests: each handle's fake source.
static void handle_capture_request(struct wl_listener *listener, void *data)
{
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request = data;
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request, request->toplevel_handle->data);
}

// ----------------------------------------------------------------- client --

struct client {
    struct wl_display *wl;
    struct wl_client *server_side;
    struct wl_registry *registry;
    struct ext_image_copy_capture_manager_v1 *copy;
    struct ext_foreign_toplevel_list_v1 *list;
    struct ext_foreign_toplevel_image_capture_source_manager_v1 *sources;
    struct zwlr_screencopy_manager_v1 *screencopy;
    struct wl_output *outputs[2];
    int output_count;
    struct ext_foreign_toplevel_handle_v1 *toplevels[2]; // the screen's, the window's
    int toplevel_count;
};

static void toplevel_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {}
static void toplevel_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {}
static void toplevel_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *title) {}
static void toplevel_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *app_id) {}
static void toplevel_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *id) {}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_listener = {
    toplevel_closed, toplevel_done, toplevel_title, toplevel_app_id, toplevel_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
    struct ext_foreign_toplevel_handle_v1 *handle)
{
    struct client *c = data;
    assert(c->toplevel_count < 2);
    c->toplevels[c->toplevel_count++] = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_listener, c);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = { list_toplevel, list_finished };

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
    uint32_t version)
{
    struct client *c = data;
    if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
        c->copy = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        c->list = wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
        ext_foreign_toplevel_list_v1_add_listener(c->list, &list_listener, c);
    } else if (strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
        c->sources = wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        c->screencopy = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 3);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && c->output_count < 2) {
        c->outputs[c->output_count++] = wl_registry_bind(registry, name, &wl_output_interface, 3);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = { handle_global, handle_global_remove };

// Both ends in one thread: the server runs whenever the client waits.
static void pump(struct client *c)
{
    for (int i = 0; i < 6; ++i) {
        if (c) {
            wl_display_flush(c->wl);
        }
        wl_event_loop_dispatch(loop, 0);
        wl_display_flush_clients(display);
        if (!c) {
            continue;
        }
        while (wl_display_prepare_read(c->wl) != 0) {
            wl_display_dispatch_pending(c->wl);
        }
        struct pollfd fd = { wl_display_get_fd(c->wl), POLLIN, 0 };
        if (poll(&fd, 1, 2) > 0) {
            wl_display_read_events(c->wl);
        } else {
            wl_display_cancel_read(c->wl);
        }
        wl_display_dispatch_pending(c->wl);
    }
}

static void connect_client(struct client *c)
{
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    memset(c, 0, sizeof(*c));
    c->server_side = wl_client_create(display, fds[0]);
    assert(c->server_side);
    c->wl = wl_display_connect_to_fd(fds[1]);
    assert(c->wl);
    c->registry = wl_display_get_registry(c->wl);
    wl_registry_add_listener(c->registry, &registry_listener, c);
    pump(c);
    pump(c);
    assert(c->copy && c->list && c->sources && c->screencopy && c->output_count == 2 && c->toplevel_count == 2);
}

static void disconnect_client(struct client *c)
{
    wl_display_disconnect(c->wl);
    pump(NULL);
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *s) {}
static void session_size(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t w, uint32_t h) {}
static void session_shm(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t format) {}
static void session_device(void *data, struct ext_image_copy_capture_session_v1 *s, struct wl_array *dev) {}
static void session_dmabuf(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t f, struct wl_array *m) {}
static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s) {}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    session_size, session_shm, session_device, session_dmabuf, session_done, session_stopped,
};

// A session on the screen's source (0) or the window's (1).
static struct ext_image_copy_capture_session_v1 *open_session(struct client *c, int which)
{
    struct ext_image_capture_source_v1 *source
        = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(c->sources, c->toplevels[which]);
    struct ext_image_copy_capture_session_v1 *session
        = ext_image_copy_capture_manager_v1_create_session(c->copy, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &session_listener, NULL);
    ext_image_capture_source_v1_destroy(source);
    pump(c);
    return session;
}

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h,
    uint32_t stride) {}
static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {}
static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns) {}
static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    ++*(int *)data;
}
static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w,
    uint32_t h) {}
static void frame_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h) {}
static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *f) {}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    frame_buffer, frame_flags, frame_ready, frame_failed, frame_damage, frame_dmabuf, frame_buffer_done,
};

static int failed_frames;

// A screencopy frame of output 0 (enabled) or 1 (disabled): asked for, never
// copied, so it stays pending.
static struct zwlr_screencopy_frame_v1 *ask_frame(struct client *c, int output)
{
    struct zwlr_screencopy_frame_v1 *frame = zwlr_screencopy_manager_v1_capture_output(c->screencopy, 0,
        c->outputs[output]);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, &failed_frames);
    pump(c);
    return frame;
}

// ---------------------------------------------------------------- checks --

static char state[4096];

static const char *report(void)
{
    struct vela_buffer json = { 0 };
    vela_capture_json(&server, &json);
    snprintf(state, sizeof(state), "%s", vela_buffer_text(&json));
    vela_buffer_finish(&json);
    return state;
}

static bool nobody(void)
{
    return strcmp(report(), "{\"clients\":[]}") == 0;
}

static int count(const char *text, const char *needle)
{
    int n = 0;
    for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle)) {
        ++n;
    }
    return n;
}

// The clock moves; the watch looks again.
static void later(struct vela_capture_watch *watch, int64_t ms)
{
    clock_ns += ms * VELA_NS_PER_MS;
    vela_capture_watch_refresh(watch);
}

// ------------------------------------------------------- other processes --

// This test as a client of its own, under another name (`--client`), on the
// display's socket (a socketpair would carry this process's credentials): a
// session on the screen's source, held until its stdin closes.
static int run_client(void)
{
    struct client c = { 0 };
    c.wl = wl_display_connect(NULL);
    assert(c.wl);
    c.registry = wl_display_get_registry(c.wl);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    wl_display_roundtrip(c.wl);
    wl_display_roundtrip(c.wl);
    struct ext_image_capture_source_v1 *source
        = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(c.sources, c.toplevels[0]);
    ext_image_copy_capture_manager_v1_create_session(c.copy, source, 0);
    wl_display_roundtrip(c.wl);
    char byte;
    while (read(STDIN_FILENO, &byte, 1) > 0) {
    }
    wl_display_disconnect(c.wl);
    return 0;
}

struct process {
    pid_t pid;
    int stdin_fd;
    char path[256];
};

// Runs this test as `name` (a copy: /proc/<pid>/exe names the file) and
// waits until its session is open.
static struct process spawn_as(const char *name)
{
    struct process p = { 0 };
    char dir[] = "/tmp/vela-capture-watch-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(p.path, sizeof(p.path), "%s/%s", dir, name);
    int in = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    int out = open(p.path, O_WRONLY | O_CREAT | O_CLOEXEC, 0700);
    assert(in >= 0 && out >= 0);
    char chunk[65536];
    for (ssize_t n; (n = read(in, chunk, sizeof(chunk))) > 0;) {
        assert(write(out, chunk, (size_t)n) == n);
    }
    close(in);
    close(out);
    int pipe_fds[2];
    assert(pipe2(pipe_fds, O_CLOEXEC) == 0);
    p.pid = fork();
    assert(p.pid >= 0);
    if (p.pid == 0) {
        dup2(pipe_fds[0], STDIN_FILENO);
        execl(p.path, name, "--client", (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[0]);
    p.stdin_fd = pipe_fds[1];
    // Serve it until its session exists.
    for (int i = 0; i < 300 && !strstr(report(), "\"pid\""); ++i) {
        wl_event_loop_dispatch(loop, 10);
        wl_display_flush_clients(display);
        clock_ns += 10 * VELA_NS_PER_MS; // each turn a little later: it counts after 1 s
        vela_capture_watch_refresh(server.capture_watch);
    }
    return p;
}

// Serve it a little more: a session that wasn't reported had its chance.
static void serve(int turns)
{
    for (int i = 0; i < turns; ++i) {
        wl_event_loop_dispatch(loop, 10);
        wl_display_flush_clients(display);
        clock_ns += 10 * VELA_NS_PER_MS;
        vela_capture_watch_refresh(server.capture_watch);
    }
}

static void stop(struct process *p)
{
    close(p->stdin_fd);
    while (waitpid(p->pid, NULL, WNOHANG) == 0) {
        wl_event_loop_dispatch(loop, 10);
        wl_display_flush_clients(display);
    }
    wl_event_loop_dispatch(loop, 0);
    unlink(p->path);
    *strrchr(p->path, '/') = '\0';
    rmdir(p->path);
}

// ------------------------------------------------------------------ main --

static void enable(struct wlr_output *output, struct wlr_allocator *allocator, struct wlr_renderer *renderer)
{
    assert(wlr_output_init_render(output, allocator, renderer));
    struct wlr_output_state on;
    wlr_output_state_init(&on);
    wlr_output_state_set_enabled(&on, true);
    wlr_output_state_set_custom_mode(&on, 320, 240, 0);
    assert(wlr_output_commit_state(output, &on));
    wlr_output_state_finish(&on);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--client") == 0) {
        return run_client();
    }
    unsetenv("VELA_CAPTURE_UNREPORTED");
    display = wl_display_create();
    loop = wl_display_get_event_loop(display);
    // A socket of its own, for the clients in other processes.
    char runtime[] = "/tmp/vela-capture-watch-runtime-XXXXXX";
    assert(mkdtemp(runtime));
    setenv("XDG_RUNTIME_DIR", runtime, 1);
    const char *socket_name = wl_display_add_socket_auto(display);
    assert(socket_name);
    setenv("WAYLAND_DISPLAY", socket_name, 1);
    struct wlr_backend *backend = wlr_multi_backend_create(loop);
    struct wlr_backend *headless = wlr_headless_backend_create(loop);
    assert(backend && headless && wlr_multi_backend_add(backend, headless));
    struct wlr_renderer *renderer = wlr_pixman_renderer_create();
    struct wlr_allocator *allocator = wlr_allocator_autocreate(headless, renderer);
    assert(renderer && allocator);
    assert(wlr_backend_start(backend));

    server.display = display;
    server.loop = loop;
    server.backend = backend;
    wl_list_init(&server.outputs);
    wl_list_init(&server.views);

    // An enabled output, the one shared, and a disabled one.
    shared_output = wlr_headless_add_output(headless, 320, 240);
    struct wlr_output *off = wlr_headless_add_output(headless, 320, 240);
    enable(shared_output, allocator, renderer);
    assert(wlr_output_init_render(off, allocator, renderer));
    wlr_output_create_global(shared_output, display);
    wlr_output_create_global(off, display);
    struct vela_output outputs[2] = { { .server = &server, .wlr = shared_output }, { .server = &server, .wlr = off } };
    wl_list_insert(&server.outputs, &outputs[0].link);
    wl_list_insert(&server.outputs, &outputs[1].link);

    // The managers, and two toplevels whose sources stand for an output's and
    // a window's.
    struct wlr_ext_image_copy_capture_manager_v1 *copy = wlr_ext_image_copy_capture_manager_v1_create(display, 1);
    struct wlr_screencopy_manager_v1 *screencopy = wlr_screencopy_manager_v1_create(display);
    struct wlr_ext_foreign_toplevel_list_v1 *list = wlr_ext_foreign_toplevel_list_v1_create(display, 1);
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *sources
        = wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(display, 1);
    struct wl_listener capture_request = { .notify = handle_capture_request };
    wl_signal_add(&sources->events.new_request, &capture_request);
    wlr_ext_image_capture_source_v1_init(&output_source, &source_impl);
    wlr_ext_image_capture_source_v1_init(&window_source, &source_impl);
    struct wlr_ext_foreign_toplevel_handle_v1_state screen_state = { .title = "screen", .app_id = "screen" };
    struct wlr_ext_foreign_toplevel_handle_v1_state window_state = { .title = "Notes", .app_id = "org.vela.Notes" };
    struct wlr_ext_foreign_toplevel_handle_v1 *screen_handle = wlr_ext_foreign_toplevel_handle_v1_create(list,
        &screen_state);
    struct wlr_ext_foreign_toplevel_handle_v1 *window_handle = wlr_ext_foreign_toplevel_handle_v1_create(list,
        &window_state);
    screen_handle->data = &output_source;
    window_handle->data = &window_source;

    // The window that is shared, as the compositor knows it.
    struct vela_view window = { .server = &server, .ext_handle = window_handle,
        .capture = (struct vela_window_capture *)&window_source };
    wl_list_insert(&server.views, &window.link);

    // Without a watch the topic says nobody.
    assert(nobody());
    vela_capture_watch_refresh(NULL);
    vela_capture_watch_destroy(NULL);

    // VELA_CAPTURE_UNREPORTED=1: nothing is followed, test-shell included.
    setenv("VELA_CAPTURE_UNREPORTED", "1", 1);
    server.capture_watch = vela_capture_watch_create(&server, copy, screencopy);
    vela_capture_watch_test_shell(server.capture_watch, "1");
    vela_capture_watch_refresh(server.capture_watch);
    assert(nobody() && notified == 0);
    vela_capture_watch_destroy(server.capture_watch);
    unsetenv("VELA_CAPTURE_UNREPORTED");

    // Without ext-image-copy-capture: screencopy only.
    vela_capture_watch_destroy(vela_capture_watch_create(&server, NULL, screencopy));

    // Connected before the watch existed (none do): never reported.
    struct client early;
    connect_client(&early);

    struct vela_capture_watch *watch = vela_capture_watch_create(&server, copy, screencopy);
    server.capture_watch = watch;
    open_session(&early, 0);
    later(watch, 2000);
    assert(nobody());

    // A session counts once it has been open for a second, frames or not.
    struct client a;
    connect_client(&a);
    struct ext_image_copy_capture_session_v1 *screen = open_session(&a, 0);
    assert(nobody());
    later(watch, 999);
    assert(nobody());
    const int before = notified;
    later(watch, 1);
    assert(strstr(report(), "\"type\":\"output\",\"name\":\"HEADLESS-1\""));
    assert(strstr(state, "\"portal\":false") && strstr(state, "\"app_id\":\"\",\"window\":\"\""));
    assert(count(state, "\"pid\":") == 1);
    assert(notified == before + 1);
    later(watch, 1000);
    assert(notified == before + 1); // nothing new: nothing sent

    // A window, and the same output again: one target each.
    struct ext_image_copy_capture_session_v1 *window_session = open_session(&a, 1);
    struct ext_image_copy_capture_session_v1 *again = open_session(&a, 0);
    later(watch, 1000);
    assert(count(report(), "\"type\":\"output\"") == 1);
    assert(strstr(state, "\"type\":\"window\",\"id\":\"") && strstr(state, "\"app_id\":\"org.vela.Notes\""));
    assert(strstr(state, "\"title\":\"Notes\""));
    // Its title follows the window.
    window_title = "Notes — draft";
    vela_capture_watch_refresh(watch);
    assert(strstr(report(), "\"title\":\"Notes — draft\""));

    // Without its identifier, and a second session on it: still one.
    struct ext_image_copy_capture_session_v1 *window_again = open_session(&a, 1);
    window.ext_handle = NULL;
    later(watch, 1000);
    assert(count(report(), "\"type\":\"window\",\"id\":\"\"") == 1);
    window.ext_handle = window_handle;
    // A source that is neither an output's nor a known window's: no target.
    wl_list_remove(&window.link);
    later(watch, 0);
    assert(!strstr(report(), "\"type\":\"window\""));
    wl_list_insert(&server.views, &window.link);

    // The client's own window names it.
    struct wlr_xdg_toplevel toplevel = { .resource = wl_resource_create(a.server_side, &wl_callback_interface, 1, 0) };
    window.xdg = &toplevel;
    vela_capture_watch_refresh(watch);
    assert(strstr(report(), "\"app_id\":\"org.vela.Notes\",\"window\":\""));
    window.xdg = NULL;

    // Closed: gone at once.
    ext_image_copy_capture_session_v1_destroy(screen);
    ext_image_copy_capture_session_v1_destroy(window_session);
    ext_image_copy_capture_session_v1_destroy(again);
    ext_image_copy_capture_session_v1_destroy(window_again);
    pump(&a);
    assert(nobody());

    // wlr-screencopy: one frame per output is a screenshot.
    struct client b;
    connect_client(&b);
    struct zwlr_screencopy_frame_v1 *first = ask_frame(&b, 0);
    struct zwlr_screencopy_frame_v1 *disabled = ask_frame(&b, 1);
    assert(failed_frames == 1); // the disabled output's fails at once
    later(watch, 5000);
    assert(nobody());
    // The same output again within 2 s: a stream.
    zwlr_screencopy_frame_v1_destroy(first);
    zwlr_screencopy_frame_v1_destroy(disabled);
    first = ask_frame(&b, 0);
    later(watch, 1500);
    struct zwlr_screencopy_frame_v1 *second = ask_frame(&b, 0);
    vela_capture_watch_refresh(watch);
    assert(strstr(report(), "\"name\":\"HEADLESS-1\"") && count(state, "\"pid\":") == 1);
    // Waiting for damage, pending: still counted long after.
    later(watch, 5000);
    assert(!nobody());
    // Its output no longer in the layout: nothing to name.
    wl_list_remove(&outputs[0].link);
    later(watch, 0);
    assert(nobody());
    wl_list_insert(&server.outputs, &outputs[0].link);
    // Two clients at once.
    struct ext_image_copy_capture_session_v1 *both = open_session(&a, 0);
    later(watch, 1000);
    assert(count(report(), "\"pid\":") == 2 && strstr(state, "]},{\"pid\":"));
    ext_image_copy_capture_session_v1_destroy(both);
    pump(&a);
    // Done (destroyed): gone 2 s after the last frame.
    zwlr_screencopy_frame_v1_destroy(first);
    zwlr_screencopy_frame_v1_destroy(second);
    pump(&b);
    later(watch, 0);
    assert(nobody());

    // One that dies with a stream and a frame not yet looked at.
    struct client e;
    connect_client(&e);
    ask_frame(&e, 0);
    ask_frame(&e, 0);
    zwlr_screencopy_manager_v1_capture_output(e.screencopy, 0, e.outputs[0]);
    wl_display_flush(e.wl);
    later(watch, 0);
    disconnect_client(&e);
    assert(nobody());

    // A client that dies while capturing takes its report with it.
    struct client c;
    connect_client(&c);
    open_session(&c, 0);
    later(watch, 1000);
    assert(!nobody());
    const int alive = notified;
    disconnect_client(&c);
    assert(nobody() && notified == alive + 1); // and the watchers hear it

    // The timer: once the second has passed, it looks by itself.
    struct client d;
    connect_client(&d);
    struct ext_image_copy_capture_session_v1 *timed = open_session(&d, 0);
    clock_ns += 1000 * VELA_NS_PER_MS;
    for (int i = 0; i < 150 && nobody(); ++i) {
        wl_event_loop_dispatch(loop, 10);
    }
    assert(!nobody());
    ext_image_copy_capture_session_v1_destroy(timed);
    pump(&d);
    assert(nobody());

    // Other processes, by their binary: the portal is said to be one.
    struct process portal = spawn_as("xdg-desktop-portal-wlr");
    assert(strstr(report(), "\"binary\":\"xdg-desktop-portal-wlr\"") && strstr(state, "\"portal\":true"));
    stop(&portal);
    assert(nobody());
    // The shell under sh: our child, and vela-shell. Another child is an app.
    server.shell.pid = getpid();
    struct process under_sh = spawn_as("vela-shell");
    serve(150);
    assert(nobody());
    stop(&under_sh);
    struct process other = spawn_as("vela-tool");
    assert(strstr(report(), "\"binary\":\"vela-tool\""));
    stop(&other);
    server.shell.pid = 0;

    // The shell is the compositor's child: never reported. (These clients
    // are this process: its pid stands for the shell's.)
    server.shell.pid = getpid();
    struct client shell;
    connect_client(&shell);
    open_session(&shell, 0);
    ask_frame(&shell, 0);
    ask_frame(&shell, 0);
    later(watch, 2000);
    assert(nobody());
    // A child of the shell that isn't vela-shell is an app like any other.
    server.shell.pid = getppid();
    struct client child;
    connect_client(&child);
    open_session(&child, 0);
    later(watch, 1000);
    assert(!nobody());
    server.shell.pid = 0;

    // test-shell: a bad pid changes nothing; this process's captures,
    // ongoing ones too, stop being reported.
    vela_capture_watch_test_shell(watch, "shell");
    vela_capture_watch_test_shell(watch, "-3");
    assert(!nobody());
    char pid[16];
    snprintf(pid, sizeof(pid), "%d", (int)getpid());
    struct zwlr_screencopy_frame_v1 *one = ask_frame(&b, 0);
    later(watch, 100);
    ask_frame(&b, 0);
    later(watch, 0);
    assert(!nobody());
    (void)one;
    vela_capture_watch_test_shell(watch, pid);
    assert(nobody());
    struct client later_shell;
    connect_client(&later_shell);
    open_session(&later_shell, 0);
    later(watch, 1000);
    assert(nobody());

    // Gone with clients still connected (as at shutdown), a frame not yet
    // looked at.
    zwlr_screencopy_manager_v1_capture_output(later_shell.screencopy, 0, later_shell.outputs[0]);
    wl_display_flush(later_shell.wl);
    wl_event_loop_dispatch(loop, 0);
    vela_capture_watch_destroy(watch);
    server.capture_watch = NULL;
    assert(nobody());

    struct client *all[] = { &a, &b, &d, &shell, &child, &later_shell, &early };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        wl_display_disconnect(all[i]->wl);
    }
    wl_display_destroy_clients(display);
    wl_list_remove(&capture_request.link);
    wlr_ext_foreign_toplevel_handle_v1_destroy(screen_handle);
    wlr_ext_foreign_toplevel_handle_v1_destroy(window_handle);
    wlr_ext_image_capture_source_v1_finish(&output_source);
    wlr_ext_image_capture_source_v1_finish(&window_source);
    wlr_backend_destroy(backend);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wl_display_destroy(display);
    rmdir(runtime);
    puts("capture watch: ok");
    return 0;
}
