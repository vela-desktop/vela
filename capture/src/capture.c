// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vela/capture.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

struct vela_capture_job {
    struct wl_shm *shm; // Borrowed
    struct ext_image_capture_source_v1 *source; // Owned
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    int fd;
    struct vela_capture_frame result;
    void *mapping;
    uint32_t selected_format;
    bool has_format;
    bool finished;
    vela_capture_done_fn done;
    void *userdata;
};

bool vela_capture_supported_format(uint32_t format)
{
    return format == WL_SHM_FORMAT_XRGB8888 ||
        format == WL_SHM_FORMAT_ARGB8888 ||
        format == WL_SHM_FORMAT_XBGR8888 ||
        format == WL_SHM_FORMAT_ABGR8888;
}

static void complete(struct vela_capture_job *job, bool ok)
{
    if (job->finished) {
        return;
    }
    job->finished = true;
    // `done` may destroy this job; do not access it afterward.
    job->done(job, ok, job->userdata);
}

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t transform)
{
    (void)frame;
    ((struct vela_capture_job *)data)->result.transform = transform;
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
    int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)data; (void)frame; (void)x; (void)y; (void)width; (void)height;
}

static void frame_presentation(void *data, struct ext_image_copy_capture_frame_v1 *frame,
    uint32_t hi, uint32_t lo, uint32_t nsec)
{
    (void)data; (void)frame; (void)hi; (void)lo; (void)nsec;
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame)
{
    (void)frame;
    complete(data, true);
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason)
{
    (void)frame; (void)reason;
    complete(data, false);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    frame_transform, frame_damage, frame_presentation, frame_ready, frame_failed
};

static void buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
    uint32_t width, uint32_t height)
{
    (void)session;
    struct vela_capture_job *job = data;
    job->result.width = width;
    job->result.height = height;
}

static void shm_format(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t format)
{
    (void)session;
    struct vela_capture_job *job = data;
    if (!job->has_format && vela_capture_supported_format(format)) {
        job->has_format = true;
        job->selected_format = format;
    }
}

static void dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session, struct wl_array *dev)
{
    (void)data; (void)session; (void)dev;
}

static void dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
    uint32_t format, struct wl_array *modifiers)
{
    (void)data; (void)session; (void)format; (void)modifiers;
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *session)
{
    struct vela_capture_job *job = data;
    if (job->frame || !job->has_format || !job->result.width || !job->result.height ||
        job->result.width > INT32_MAX / 4) {
        if (!job->frame) complete(job, false);
        return;
    }

    const uint32_t stride = job->result.width * 4u;
    if (job->result.height > SIZE_MAX / stride ||
        (size_t)stride * job->result.height > INT32_MAX) {
        complete(job, false);
        return;
    }

    const size_t length = (size_t)stride * job->result.height;
    job->fd = memfd_create("vela-capture", MFD_CLOEXEC);
    if (job->fd < 0 || ftruncate(job->fd, (off_t)length) < 0) {
        complete(job, false);
        return;
    }
    job->mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, job->fd, 0);
    if (job->mapping == MAP_FAILED) {
        job->mapping = NULL;
        complete(job, false);
        return;
    }
    job->pool = wl_shm_create_pool(job->shm, job->fd, (int32_t)length);
    if (!job->pool) {
        complete(job, false);
        return;
    }
    job->buffer = wl_shm_pool_create_buffer(job->pool, 0, job->result.width, job->result.height,
        stride, job->selected_format);
    if (!job->buffer) {
        complete(job, false);
        return;
    }

    job->result.pixels = job->mapping;
    job->result.length = length;
    job->result.stride = stride;
    job->result.format = job->selected_format;
    job->result.transform = WL_OUTPUT_TRANSFORM_NORMAL;

    job->frame = ext_image_copy_capture_session_v1_create_frame(session);
    if (!job->frame) {
        complete(job, false);
        return;
    }
    ext_image_copy_capture_frame_v1_add_listener(job->frame, &frame_listener, job);
    ext_image_copy_capture_frame_v1_attach_buffer(job->frame, job->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(job->frame, 0, 0,
        (int32_t)job->result.width, (int32_t)job->result.height);
    ext_image_copy_capture_frame_v1_capture(job->frame);
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    complete(data, false);
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    buffer_size, shm_format, dmabuf_device, dmabuf_format, session_done, session_stopped
};

struct vela_capture_job *vela_capture_begin(struct wl_shm *shm,
    struct ext_image_copy_capture_manager_v1 *manager,
    struct ext_image_capture_source_v1 *source,
    vela_capture_done_fn done, void *userdata)
{
    if (!shm || !manager || !source || !done) return NULL;
    struct vela_capture_job *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->fd = -1;
    job->shm = shm;
    job->done = done;
    job->userdata = userdata;
    job->session = ext_image_copy_capture_manager_v1_create_session(manager, source, 0);
    if (!job->session) {
        free(job);
        return NULL;
    }
    job->source = source;
    ext_image_copy_capture_session_v1_add_listener(job->session, &session_listener, job);
    return job;
}

const struct vela_capture_frame *vela_capture_pixels(const struct vela_capture_job *job)
{
    return job && job->finished && job->result.pixels ? &job->result : NULL;
}

void vela_capture_end(struct vela_capture_job *job)
{
    if (!job) return;
    if (job->frame) ext_image_copy_capture_frame_v1_destroy(job->frame);
    if (job->session) ext_image_copy_capture_session_v1_destroy(job->session);
    if (job->source) ext_image_capture_source_v1_destroy(job->source);
    if (job->buffer) wl_buffer_destroy(job->buffer);
    if (job->pool) wl_shm_pool_destroy(job->pool);
    if (job->mapping) munmap(job->mapping, job->result.length);
    if (job->fd >= 0) close(job->fd);
    free(job);
}
