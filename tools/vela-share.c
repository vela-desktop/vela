// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-share: captures like the apps that share the screen, for the tests of
// docs/screen-sharing.md (tests/functional/test_screen_sharing.py).
//
//   vela-share ext-output [N]          one ext-image-copy-capture session on
//                                      output N (0), frame after frame, like
//                                      xdg-desktop-portal-wlr
//   vela-share ext-window IDENTIFIER   the same on a window (its
//                                      ext-foreign-toplevel identifier)
//   vela-share screencopy [N]          wlr-screencopy frames of output N, one
//                                      after the other, like wf-recorder
//   vela-share screencopy-once         one wlr-screencopy frame of every
//                                      output at once, then exits, like grim
//
// Prints "sharing" once the first frame is in, and runs until stdin closes
// (or SIGTERM); with --forever first, only until SIGTERM (as a startup command,
// whose stdin is the compositor's). VELA_SHARE_INTERVAL_MS waits between
// frames (default 16); VELA_SHARE_FRAMES=N stops asking for frames after N,
// staying connected. VELA_SHARE_DAMAGE=1 asks screencopy for frames with
// damage (copy_with_damage): each waits until the output changes.
// VELA_SHARE_KEEP=1 never destroys a finished screencopy frame, like a lazy
// client. Lines on stdin: "window" opens a window of its own (app id
// org.vela.Share), "close" closes it, as an app that shares does.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define MAX_OUTPUTS 8

static struct wl_display *display;
static struct wl_shm *shm;
static struct wl_output *outputs[MAX_OUTPUTS];
static int output_count;
static struct ext_output_image_capture_source_manager_v1 *output_sources;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_sources;
static struct ext_image_copy_capture_manager_v1 *copy_manager;
static struct ext_foreign_toplevel_list_v1 *toplevel_list;
static struct zwlr_screencopy_manager_v1 *screencopy;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;

static const char *wanted_identifier;
static struct ext_foreign_toplevel_handle_v1 *wanted_toplevel;
static int interval_ms = 16;
static int frames_left = -1; // -1: no limit
static bool with_damage, keep_frames;
static bool announced, failed;
static int64_t next_frame_ns; // 0: none waiting

static int64_t now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

// One more frame in: false when the limit says no more.
static bool another(void)
{
    if (frames_left > 0) {
        --frames_left;
    }
    return frames_left != 0;
}

static void announce(void)
{
    if (!announced) {
        announced = true;
        printf("sharing\n");
        fflush(stdout);
    }
}

// ----------------------------------------------------------------- buffers --

struct buffer {
    struct wl_buffer *wl;
    uint32_t width, height, stride, format;
};

static bool make_buffer(struct buffer *buffer, uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
    if (buffer->wl && buffer->width == width && buffer->height == height && buffer->stride == stride
        && buffer->format == format) {
        return true;
    }
    if (buffer->wl) {
        wl_buffer_destroy(buffer->wl);
        buffer->wl = NULL;
    }
    size_t size = (size_t)stride * height;
    int fd = memfd_create("vela-share", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    buffer->wl = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height, (int32_t)stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->format = format;
    return true;
}

// ------------------------------------------------- ext-image-copy-capture --

static struct ext_image_copy_capture_session_v1 *session;
static struct ext_image_copy_capture_frame_v1 *frame;
static struct buffer ext_buffer;
static uint32_t ext_width, ext_height, ext_format = UINT32_MAX;

static void ext_capture(void);

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t transform) {}
static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y, int32_t w,
    int32_t h) {}
static void frame_time(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns) {}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f)
{
    ext_image_copy_capture_frame_v1_destroy(frame);
    frame = NULL;
    announce();
    if (another()) {
        next_frame_ns = now_ns() + (int64_t)interval_ms * 1000000;
    }
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason)
{
    ext_image_copy_capture_frame_v1_destroy(frame);
    frame = NULL;
    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
        fprintf(stderr, "vela-share: the session stopped\n");
        failed = true;
        return;
    }
    next_frame_ns = now_ns() + (int64_t)interval_ms * 1000000; // new constraints: try again
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void session_size(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t width, uint32_t height)
{
    ext_width = width;
    ext_height = height;
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t format)
{
    if (ext_format == UINT32_MAX || format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ARGB8888) {
        ext_format = format;
    }
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *s, struct wl_array *dev) {}
static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t format,
    struct wl_array *modifiers) {}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *s)
{
    if (!frame && !next_frame_ns) {
        ext_capture();
    }
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s)
{
    fprintf(stderr, "vela-share: the session stopped\n");
    failed = true;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_size,
    .shm_format = session_shm_format,
    .dmabuf_device = session_dmabuf_device,
    .dmabuf_format = session_dmabuf_format,
    .done = session_done,
    .stopped = session_stopped,
};

static void ext_capture(void)
{
    if (ext_width == 0 || ext_format == UINT32_MAX
        || !make_buffer(&ext_buffer, ext_format, ext_width, ext_height, ext_width * 4)) {
        return;
    }
    frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, ext_buffer.wl);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (int32_t)ext_width, (int32_t)ext_height);
    ext_image_copy_capture_frame_v1_capture(frame);
}

static void ext_start(struct ext_image_capture_source_v1 *source)
{
    session = ext_image_copy_capture_manager_v1_create_session(copy_manager, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &session_listener, NULL);
}

// ---------------------------------------------------------- the toplevels --

static void toplevel_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {}
static void toplevel_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {}
static void toplevel_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *title) {}
static void toplevel_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *app_id) {}

static void toplevel_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *identifier)
{
    if (wanted_identifier && strcmp(identifier, wanted_identifier) == 0) {
        wanted_toplevel = handle;
    }
}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_listener = {
    .closed = toplevel_closed,
    .done = toplevel_done,
    .title = toplevel_title,
    .app_id = toplevel_app_id,
    .identifier = toplevel_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
    struct ext_foreign_toplevel_handle_v1 *handle)
{
    ext_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_listener, NULL);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
    .toplevel = list_toplevel,
    .finished = list_finished,
};

// --------------------------------------------------------- wlr-screencopy --

struct screencopy_frame {
    struct zwlr_screencopy_frame_v1 *wl;
    struct buffer buffer;
    uint32_t format, width, height, stride;
    bool done;
};

static struct screencopy_frame screencopy_frames[MAX_OUTPUTS];
static int screencopy_output = -1; // -1: every output, once
static int screencopy_left;

static void sc_buffer(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t width,
    uint32_t height, uint32_t stride)
{
    struct screencopy_frame *frame = data;
    frame->format = format;
    frame->width = width;
    frame->height = height;
    frame->stride = stride;
}

static void sc_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {}
static void sc_damage(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w,
    uint32_t h) {}
static void sc_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w,
    uint32_t h) {}

static void sc_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    struct screencopy_frame *frame = data;
    if (!make_buffer(&frame->buffer, frame->format, frame->width, frame->height, frame->stride)) {
        failed = true;
        return;
    }
    if (with_damage) {
        zwlr_screencopy_frame_v1_copy_with_damage(f, frame->buffer.wl);
    } else {
        zwlr_screencopy_frame_v1_copy(f, frame->buffer.wl);
    }
}

static void sc_finish(struct screencopy_frame *frame)
{
    if (!keep_frames) {
        zwlr_screencopy_frame_v1_destroy(frame->wl);
    }
    frame->wl = NULL;
    if (screencopy_output < 0) {
        --screencopy_left;
    } else if (another()) {
        next_frame_ns = now_ns() + (int64_t)interval_ms * 1000000;
    }
}

static void sc_ready(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns)
{
    announce();
    sc_finish(data);
}

static void sc_failed(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    fprintf(stderr, "vela-share: a screencopy frame failed\n");
    sc_finish(data);
}

static const struct zwlr_screencopy_frame_v1_listener sc_listener = {
    .buffer = sc_buffer,
    .flags = sc_flags,
    .ready = sc_ready,
    .failed = sc_failed,
    .damage = sc_damage,
    .linux_dmabuf = sc_linux_dmabuf,
    .buffer_done = sc_buffer_done,
};

static void sc_capture(int index)
{
    struct screencopy_frame *frame = &screencopy_frames[index];
    frame->wl = zwlr_screencopy_manager_v1_capture_output(screencopy, 0, outputs[index]);
    zwlr_screencopy_frame_v1_add_listener(frame->wl, &sc_listener, frame);
}

// ----------------------------------------------------------- its window --

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct buffer window_buffer;

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void surface_configure(void *data, struct xdg_surface *xdg, uint32_t serial)
{
    xdg_surface_ack_configure(xdg, serial);
    if (make_buffer(&window_buffer, WL_SHM_FORMAT_XRGB8888, 64, 64, 64 * 4)) {
        wl_surface_attach(surface, window_buffer.wl, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, 64, 64);
    }
    wl_surface_commit(surface);
}

static const struct xdg_surface_listener surface_listener = { .configure = surface_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {}
static void toplevel_close(void *data, struct xdg_toplevel *t) {}

static const struct xdg_toplevel_listener window_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void open_window(void)
{
    if (surface || !compositor || !wm_base) {
        return;
    }
    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &window_listener, NULL);
    xdg_toplevel_set_app_id(toplevel, "org.vela.Share");
    xdg_toplevel_set_title(toplevel, "vela-share");
    wl_surface_commit(surface);
}

static void close_window(void)
{
    if (surface) {
        xdg_toplevel_destroy(toplevel);
        xdg_surface_destroy(xdg_surface);
        wl_surface_destroy(surface);
        surface = NULL;
    }
}

// A line on stdin.
static void command(const char *line)
{
    if (strcmp(line, "window") == 0) {
        open_window();
    } else if (strcmp(line, "close") == 0) {
        close_window();
    }
}

// --------------------------------------------------------------- registry --

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
    uint32_t version)
{
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && output_count < MAX_OUTPUTS) {
        outputs[output_count++] = wl_registry_bind(registry, name, &wl_output_interface, 1);
    } else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
        output_sources = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
        toplevel_sources
            = wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
        copy_manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        toplevel_list = wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
        ext_foreign_toplevel_list_v1_add_listener(toplevel_list, &list_listener, NULL);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        screencopy = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, version < 3 ? version : 3);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static int usage(void)
{
    fprintf(stderr, "usage: vela-share [--forever] ext-output [N] | ext-window IDENTIFIER | screencopy [N]"
                    " | screencopy-once\n");
    return 2;
}

int main(int argc, char **argv)
{
    bool forever = argc > 1 && strcmp(argv[1], "--forever") == 0;
    if (forever) {
        ++argv;
        --argc;
    }
    if (argc < 2) {
        return usage();
    }
    const char *mode = argv[1];
    const char *frames = getenv("VELA_SHARE_FRAMES");
    if (frames && *frames) {
        frames_left = atoi(frames);
    }
    const char *damage = getenv("VELA_SHARE_DAMAGE");
    with_damage = damage && strcmp(damage, "1") == 0;
    const char *keep = getenv("VELA_SHARE_KEEP");
    keep_frames = keep && strcmp(keep, "1") == 0;
    const char *interval = getenv("VELA_SHARE_INTERVAL_MS");
    if (interval && *interval) {
        interval_ms = atoi(interval);
    }
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "vela-share: no Wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wanted_identifier = strcmp(mode, "ext-window") == 0 && argc > 2 ? argv[2] : NULL;
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); // the toplevels' identifiers
    const int index = argc > 2 && !wanted_identifier ? atoi(argv[2]) : 0;
    if (!shm || output_count == 0 || index < 0 || index >= output_count) {
        fprintf(stderr, "vela-share: no such output\n");
        return 1;
    }

    if (strcmp(mode, "ext-output") == 0) {
        if (!copy_manager || !output_sources) {
            fprintf(stderr, "vela-share: no ext-image-copy-capture\n");
            return 1;
        }
        ext_start(ext_output_image_capture_source_manager_v1_create_source(output_sources, outputs[index]));
    } else if (strcmp(mode, "ext-window") == 0) {
        if (!copy_manager || !toplevel_sources || !wanted_toplevel) {
            fprintf(stderr, "vela-share: no such window\n");
            return 1;
        }
        ext_start(ext_foreign_toplevel_image_capture_source_manager_v1_create_source(toplevel_sources,
            wanted_toplevel));
    } else if (strcmp(mode, "screencopy") == 0 || strcmp(mode, "screencopy-once") == 0) {
        if (!screencopy) {
            fprintf(stderr, "vela-share: no wlr-screencopy\n");
            return 1;
        }
        if (strcmp(mode, "screencopy") == 0) {
            screencopy_output = index;
            sc_capture(index);
        } else {
            screencopy_left = output_count;
            for (int i = 0; i < output_count; ++i) {
                sc_capture(i);
            }
        }
    } else {
        return usage();
    }

    struct pollfd fds[2] = {
        { .fd = wl_display_get_fd(display), .events = POLLIN },
        // Once: done when every frame is, whatever stdin is.
        { .fd = forever || strcmp(mode, "screencopy-once") == 0 ? -1 : STDIN_FILENO, .events = POLLIN },
    };
    while (!failed) {
        if (screencopy_output < 0 && strcmp(mode, "screencopy-once") == 0 && screencopy_left == 0) {
            break;
        }
        wl_display_flush(display);
        int timeout = -1;
        if (next_frame_ns) {
            int64_t wait = (next_frame_ns - now_ns()) / 1000000;
            timeout = wait < 0 ? 0 : (int)wait;
        }
        if (poll(fds, 2, timeout) < 0 && errno != EINTR) {
            break;
        }
        if (fds[1].revents) {
            static char line[64];
            static size_t length;
            char byte;
            if (read(STDIN_FILENO, &byte, 1) <= 0) {
                break; // stdin closed: the test is done with us
            }
            if (byte == '\n') {
                line[length] = '\0';
                command(line);
                length = 0;
            } else if (length < sizeof(line) - 1) {
                line[length++] = byte;
            }
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_dispatch(display) < 0) {
                break;
            }
        } else if (fds[0].revents & (POLLERR | POLLHUP)) {
            break;
        }
        if (next_frame_ns && now_ns() >= next_frame_ns) {
            next_frame_ns = 0;
            if (session) {
                ext_capture();
            } else if (screencopy_output >= 0) {
                sc_capture(screencopy_output);
            }
        }
    }
    wl_display_disconnect(display);
    return failed ? 1 : 0;
}
