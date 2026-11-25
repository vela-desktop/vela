// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scene/ready.h"

#include "alloc/alloc.h"
#include "listen.h"
#include "util.h"

#include <linux/dma-buf.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>
#include <xf86drm.h>

struct gate;
struct held;

// Implicit: one sync_file per dmabuf, in the event loop.
struct held_file {
    struct held *owner;
    int fd;
    struct wl_event_source *source;
};

// A held commit: applied when all its fences have signaled.
struct held {
    struct wl_list link; // gate.held, in commit order
    struct gate *gate;
    uint32_t seq;
    bool locked;
    int remaining; // fences not signaled yet
    // Explicit: the acquire point on the app's timeline.
    struct wlr_drm_syncobj_timeline_waiter waiter;
    bool waiting;
    struct held_file files[WLR_DMABUF_MAX_PLANES];
    int file_count;
};

// The per-surface state, hung as an addon on the wlr_surface.
struct gate {
    struct wlr_addon addon;
    struct vela_ready *ready;
    struct wlr_surface *surface;
    struct wl_listener client_commit;
    struct wl_list held; // struct held.link
};

static void disarm(struct held_file *file)
{
    if (file->source) {
        wl_event_source_remove(file->source);
        file->source = NULL;
    }
    if (file->fd >= 0) {
        close(file->fd);
        file->fd = -1;
    }
}

// Stops waiting and frees; the commit, if held, stays with wlroots.
static void held_destroy(struct held *held)
{
    if (held->waiting) {
        wlr_drm_syncobj_timeline_waiter_finish(&held->waiter);
    }
    for (int i = 0; i < held->file_count; ++i) {
        disarm(&held->files[i]);
    }
    if (held->locked) {
        --held->gate->ready->held_now;
    }
    wl_list_remove(&held->link);
    free(held);
}

// The surface dies with its states queued: stop waiting without unlocking them
// (wlroots throws them away).
static void handle_surface_destroy(struct wlr_addon *addon)
{
    struct gate *gate = wl_container_of(addon, gate, addon);
    struct held *held, *tmp;
    wl_list_for_each_safe (held, tmp, &gate->held, link) {
        held_destroy(held);
    }
    wl_list_remove(&gate->client_commit.link);
    wlr_addon_finish(addon);
    free(gate);
}

static const struct wlr_addon_interface gate_addon = {
    .name = "vela-ready-commits",
    .destroy = handle_surface_destroy,
};

// One fence less; at the last one the commit applies.
static void signaled(struct held *held)
{
    if (--held->remaining > 0) {
        return;
    }
    struct wlr_surface *surface = held->gate->surface;
    uint32_t seq = held->seq;
    // Removed first, then unlocked: applying the commit runs the other
    // handlers (mapping, damage, frame), and this held must be gone by then.
    held_destroy(held);
    wlr_surface_unlock_cached(surface, seq);
}

static void handle_syncobj_ready(struct wlr_drm_syncobj_timeline_waiter *waiter)
{
    struct held *held = wl_container_of(waiter, held, waiter);
    wlr_drm_syncobj_timeline_waiter_finish(waiter); // wlroots allows it in its callback
    held->waiting = false;
    signaled(held);
}

static int handle_file_ready(int fd, uint32_t mask, void *data)
{
    struct held_file *file = data;
    struct held *held = file->owner;
    // A signaled sync_file stays readable: it must be detached at once, or the
    // event loop would count it again.
    disarm(file);
    signaled(held);
    return 0;
}

// The linux-drm-syncobj-v1 state of the incoming commit. wlroots exposes only
// the current one (wlr_linux_drm_syncobj_v1_get_surface_state); the incoming
// one is the state of the same synced object for surface->pending. The object
// is recognized because its current state is the one wlroots returns. The list
// of synced objects is private in wlroots 0.20: if it changes, compilation
// stops here.
static struct wlr_linux_drm_syncobj_surface_v1_state *pending_syncobj(struct wlr_surface *surface)
{
    struct wlr_linux_drm_syncobj_surface_v1_state *current = wlr_linux_drm_syncobj_v1_get_surface_state(surface);
    if (!current) {
        return NULL;
    }
    struct wlr_surface_synced *synced;
    wl_list_for_each (synced, &surface->WLR_PRIVATE.synced, link) {
        if (wlr_surface_synced_get_state(synced, &surface->current) == current) {
            return wlr_surface_synced_get_state(synced, &surface->pending);
        }
    }
    return NULL;
}

// Explicit: wait for the point to be signaled (wlroots only waits for it to
// materialize). false if it's already ready or can't be waited for.
static bool wait_syncobj(struct held *held, struct wlr_linux_drm_syncobj_surface_v1_state *sync,
    struct wl_event_loop *loop)
{
    bool ready = false;
    if (!wlr_drm_syncobj_timeline_check(sync->acquire_timeline, sync->acquire_point,
            DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &ready)
        || ready) {
        return false;
    }
    if (!wlr_drm_syncobj_timeline_waiter_init(&held->waiter, sync->acquire_timeline, sync->acquire_point, 0, loop,
            handle_syncobj_ready)) {
        return false; // the frame will wait for the GPU, as before
    }
    held->waiting = true;
    held->remaining = 1;
    return true;
}

// Implicit: the dmabuf write fences, those readers must wait for, one per
// distinct descriptor.
static void wait_dmabuf(struct held *held, struct wlr_buffer *buffer, struct wl_event_loop *loop)
{
    struct wlr_dmabuf_attributes dmabuf;
    if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
        return; // shared memory: already ready
    }
    for (int i = 0; i < dmabuf.n_planes; ++i) {
        int fd = dmabuf.fd[i];
        bool seen = false;
        for (int j = 0; j < i && !seen; ++j) {
            seen = dmabuf.fd[j] == fd;
        }
        if (seen) {
            continue;
        }
        struct dma_buf_export_sync_file request = { .flags = DMA_BUF_SYNC_READ, .fd = -1 };
        if (ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request) != 0 || request.fd < 0) {
            continue;
        }
        struct pollfd poll_fd = { .fd = request.fd, .events = POLLIN };
        if (poll(&poll_fd, 1, 0) > 0) {
            close(request.fd); // already signaled
            continue;
        }
        struct held_file *file = &held->files[held->file_count];
        *file = (struct held_file) { held, request.fd, NULL };
        file->source = wl_event_loop_add_fd(loop, request.fd, WL_EVENT_READABLE, handle_file_ready, file);
        if (!file->source) {
            close(request.fd);
            continue;
        }
        ++held->file_count;
        ++held->remaining;
    }
}

static void handle_client_commit(struct wl_listener *listener, void *data)
{
    struct gate *gate = wl_container_of(listener, gate, client_commit);
    struct wlr_surface *surface = gate->surface;
    const struct wlr_surface_state *pending = &surface->pending;
    if (!(pending->committed & WLR_SURFACE_STATE_BUFFER) || !pending->buffer) {
        return;
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(wl_client_get_display(wl_resource_get_client(surface->resource)));
    struct held *held = VELA_NEW(struct held);
    held->gate = gate;
    wl_list_init(&held->link);

    struct wlr_linux_drm_syncobj_surface_v1_state *sync = pending_syncobj(surface);
    if (sync && sync->acquire_timeline) {
        if (!wait_syncobj(held, sync, loop)) {
            free(held);
            return;
        }
    } else {
        wait_dmabuf(held, pending->buffer, loop);
        if (held->remaining == 0) {
            free(held);
            return;
        }
    }

    held->seq = wlr_surface_lock_pending(surface);
    held->locked = true;
    ++gate->ready->held_total;
    ++gate->ready->held_now;
    wl_list_insert(gate->held.prev, &held->link);
}

static void handle_new_surface(struct wl_listener *listener, void *data)
{
    struct vela_ready *ready = wl_container_of(listener, ready, new_surface);
    struct wlr_surface *surface = data;
    struct gate *gate = VELA_NEW(struct gate);
    gate->ready = ready;
    gate->surface = surface;
    wl_list_init(&gate->held);
    wlr_addon_init(&gate->addon, &surface->addons, NULL, &gate_addon);
    vela_listen(&surface->events.client_commit, &gate->client_commit, handle_client_commit);
}

void vela_ready_init(struct vela_ready *ready, struct wlr_compositor *compositor)
{
    ready->held_total = 0;
    ready->held_now = 0;
    ready->enabled = !vela_env_off("VELA_READY_WAIT");
    wl_list_init(&ready->new_surface.link);
    if (!ready->enabled) {
        wlr_log(WLR_INFO, "VELA_READY_WAIT=0: frames wait for the apps' GPU");
        return;
    }
    vela_listen(&compositor->events.new_surface, &ready->new_surface, handle_new_surface);
}

void vela_ready_finish(struct vela_ready *ready)
{
    vela_unlisten(&ready->new_surface);
}
