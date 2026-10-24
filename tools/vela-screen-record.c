// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-screen-record: records an output frame by frame, for videos
// (scripts/record-before-after.py).
//
// Usage: vela-screen-record DIRECTORY [X Y WIDTH HEIGHT]
//
// Writes, in an existing DIRECTORY:
//   frames.raw  the pixels of each frame, one after the other: 4 bytes per
//               pixel, rows of WIDTH pixels, no padding;
//   frames.txt  "WIDTH HEIGHT FORMAT" (FFmpeg's pixel format, bgr0 or rgb0),
//               then one line per frame: its presentation time, in seconds
//               of CLOCK_MONOTONIC.
// A frame is written each time the output shows something new, so a still
// screen writes nothing. With coordinates, only that area (output pixels).
// Prints "recording" once the first frame is in, and stops on SIGINT or
// SIGTERM. VELA_SCREEN_RECORD_OUTPUT selects an output;
// VELA_SCREEN_RECORD_DOWNSCALE=N averages blocks of N×N pixels (a quarter of
// the data with 2, for full outputs at high refresh rates).
//
// Unlike vela-screen-dump it keeps one ext-image-copy-capture session for the
// whole recording, with two buffers: the compositor fills one while the other
// is written out.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "vela/capture.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

struct buffer {
    int fd;
    uint8_t* data;
    size_t length;
    struct wl_buffer* wl;
};

static struct wl_shm* shm;
static struct wl_output* output;
// Keep every advertised output until its name arrives. Registry order alone
// does not identify a monitor across compositor versions or restarts.
struct recording_output {
    struct wl_output* wl;
    char* name;
    struct recording_output* next;
};
static struct recording_output* outputs;
static struct ext_output_image_capture_source_manager_v1* sources;
static struct ext_image_copy_capture_manager_v1* manager;
static struct ext_image_copy_capture_session_v1* session;
static struct ext_image_copy_capture_frame_v1* frame;

// Constraints as the session proposes them, and as the buffers were made.
static uint32_t proposedWidth, proposedHeight, proposedFormat;
static bool hasProposedFormat;
static bool constraintsCollecting; // a fresh session constraint batch
static uint32_t width, height, format;
static struct buffer buffers[2];
static int current; // the buffer the compositor is filling
static bool reallocate; // the constraints changed: new buffers before the next frame
static bool constraintsWaiting; // the session's done came while a frame was in flight

static uint32_t transform;
static uint64_t presented; // ns; 0 until this frame's presentation_time
static int regionX, regionY, regionW, regionH; // regionW == 0: the whole output
static int downscale = 1;
static int outW, outH; // the written frame
static uint8_t* row; // one written row, when downscaling
static FILE* raw;
static FILE* times;
static unsigned long frames;
static unsigned captureFailures; // consecutive capture failures, including renegotiation
static volatile sig_atomic_t stopping;
static bool failed;

static void stop(int signal)
{
    (void)signal;
    stopping = 1;
}

static const char* ffmpegFormat(uint32_t shmFormat)
{
    // In memory: XRGB/ARGB8888 = B,G,R,A; XBGR/ABGR8888 = R,G,B,A. Alpha is
    // the output's, meaningless here.
    return shmFormat == WL_SHM_FORMAT_XRGB8888 || shmFormat == WL_SHM_FORMAT_ARGB8888 ? "bgr0" : "rgb0";
}

static void freeBuffers(void)
{
    for (int i = 0; i < 2; ++i) {
        struct buffer* b = &buffers[i];
        if (b->wl) wl_buffer_destroy(b->wl);
        if (b->data) munmap(b->data, b->length);
        if (b->fd >= 0) close(b->fd);
        *b = (struct buffer) { .fd = -1 };
    }
}

static bool allocateBuffers(void)
{
    freeBuffers();
    if (!hasProposedFormat || !proposedWidth || !proposedHeight || proposedWidth > INT32_MAX / 4) {
        return false;
    }
    const uint32_t stride = proposedWidth * 4;
    if ((uint64_t)stride * proposedHeight > INT32_MAX) {
        return false;
    }
    const size_t length = (size_t)stride * proposedHeight;
    for (int i = 0; i < 2; ++i) {
        struct buffer* b = &buffers[i];
        b->fd = memfd_create("vela-screen-record", MFD_CLOEXEC);
        if (b->fd < 0 || ftruncate(b->fd, (off_t)length) < 0) {
            return false;
        }
        void* data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
        if (data == MAP_FAILED) {
            return false;
        }
        b->data = data;
        b->length = length;
        struct wl_shm_pool* pool = wl_shm_create_pool(shm, b->fd, (int32_t)length);
        b->wl = wl_shm_pool_create_buffer(pool, 0, (int32_t)proposedWidth, (int32_t)proposedHeight, (int32_t)stride,
            proposedFormat);
        wl_shm_pool_destroy(pool);
    }
    width = proposedWidth;
    height = proposedHeight;
    format = proposedFormat;
    reallocate = false;
    return true;
}

static const struct ext_image_copy_capture_frame_v1_listener frameListener;

static void beginConstraints(void)
{
    if (constraintsCollecting) return;
    constraintsCollecting = true;
    proposedWidth = proposedHeight = 0;
    hasProposedFormat = false;
    proposedFormat = 0;
}

static void captureNext(void)
{
    presented = 0;
    frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &frameListener, NULL);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffers[current].wl);
    // The whole buffer: two buffers take turns, so each misses the other's
    // changes.
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (int32_t)width, (int32_t)height);
    ext_image_copy_capture_frame_v1_capture(frame);
}

/** Between frames: new buffers if the constraints changed, then the next capture. */
static void resume(void)
{
    constraintsWaiting = false;
    if (!width || reallocate) {
        if (frames && (proposedWidth != width || proposedHeight != height || proposedFormat != format)) {
            fprintf(stderr, "vela-screen-record: output size or pixel format changed during recording\n");
            failed = true;
            stopping = 1;
            return;
        }
        if (!allocateBuffers()) {
            fprintf(stderr, "vela-screen-record: can't allocate the capture buffers\n");
            failed = true;
            stopping = 1;
            return;
        }
    }
    captureNext();
}

/** The first frame fixes the written size; later ones must keep it. */
static bool startFiles(void)
{
    if (regionW == 0) {
        regionW = (int)width;
        regionH = (int)height;
    }
    if (regionX < 0 || regionY < 0 || regionW <= 0 || regionH <= 0 ||
        (int64_t)regionX + regionW > width || (int64_t)regionY + regionH > height) {
        fprintf(stderr, "vela-screen-record: region outside the output (%ux%u)\n", width, height);
        return false;
    }
    outW = regionW / downscale;
    outH = regionH / downscale;
    if (outW == 0 || outH == 0) {
        fprintf(stderr, "vela-screen-record: region smaller than the downscale\n");
        return false;
    }
    if (downscale > 1 && !(row = malloc((size_t)outW * 4))) {
        return false;
    }
    fprintf(times, "%d %d %s\n", outW, outH, ffmpegFormat(format));
    return true;
}

static bool writeFrame(const struct buffer* b, uint64_t when)
{
    const size_t stride = (size_t)width * 4;
    for (int y = 0; y < outH; ++y) {
        const uint8_t* source = b->data + (size_t)(regionY + y * downscale) * stride + (size_t)regionX * 4;
        if (downscale == 1) {
            fwrite(source, 4, (size_t)outW, raw);
            continue;
        }
        const unsigned n = (unsigned)(downscale * downscale);
        for (int x = 0; x < outW; ++x) {
            unsigned sum[4] = { 0, 0, 0, 0 };
            for (int dy = 0; dy < downscale; ++dy) {
                const uint8_t* p = source + (size_t)dy * stride + (size_t)x * downscale * 4;
                for (int dx = 0; dx < downscale; ++dx, p += 4) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                }
            }
            for (int c = 0; c < 4; ++c) {
                row[(size_t)x * 4 + c] = (uint8_t)((sum[c] + n / 2) / n);
            }
        }
        fwrite(row, 4, (size_t)outW, raw);
    }
    fprintf(times, "%llu.%09llu\n", (unsigned long long)(when / 1000000000u),
        (unsigned long long)(when % 1000000000u));
    return !ferror(raw) && !ferror(times);
}

static void frameTransform(void* data, struct ext_image_copy_capture_frame_v1* f, uint32_t value)
{
    (void)data; (void)f;
    transform = value;
}

static void frameDamage(void* data, struct ext_image_copy_capture_frame_v1* f,
    int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)data; (void)f; (void)x; (void)y; (void)w; (void)h;
}

static void framePresentation(void* data, struct ext_image_copy_capture_frame_v1* f,
    uint32_t hi, uint32_t lo, uint32_t nsec)
{
    (void)data; (void)f;
    presented = (((uint64_t)hi << 32) | lo) * 1000000000u + nsec;
}

static void frameReady(void* data, struct ext_image_copy_capture_frame_v1* f)
{
    (void)data;
    ext_image_copy_capture_frame_v1_destroy(f);
    frame = NULL;
    uint64_t when = presented;
    if (!when) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        when = (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
    }
    if (transform != WL_OUTPUT_TRANSFORM_NORMAL) {
        fprintf(stderr, "vela-screen-record: only outputs without a transform\n");
        failed = true;
        stopping = 1;
        return;
    }
    const struct buffer* filled = &buffers[current];
    if (frames == 0 && !startFiles()) {
        failed = true;
        stopping = 1;
        return;
    }
    captureFailures = 0;
    // Ask for the next frame first: the compositor fills the other buffer
    // while this one is written. New buffers wait until it's written.
    if (!stopping && !reallocate) {
        current ^= 1;
        captureNext();
    }
    if (!writeFrame(filled, when)) {
        perror("vela-screen-record");
        failed = true;
        stopping = 1;
        return;
    }
    if (frames++ == 0) {
        printf("recording\n");
        fflush(stdout);
    }
    if (!stopping && reallocate && constraintsWaiting) {
        resume();
    } else if (!reallocate) {
        // The previous constraints were still valid: no deferred reallocation.
        constraintsWaiting = false;
    }
}

static void frameFailed(void* data, struct ext_image_copy_capture_frame_v1* f, uint32_t reason)
{
    (void)data;
    ext_image_copy_capture_frame_v1_destroy(f);
    frame = NULL;
    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
        failed = !stopping;
        stopping = 1;
        return;
    }
    if (++captureFailures >= 5) {
        fprintf(stderr, "vela-screen-record: capture failed 5 times (last reason %u)\n", reason);
        failed = true;
        stopping = 1;
        return;
    }
    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
        reallocate = true;
        if (constraintsWaiting) {
            resume(); // otherwise wait for the next session.done
        }
    } else if (!stopping) {
        captureNext();
    }
}

static const struct ext_image_copy_capture_frame_v1_listener frameListener = {
    frameTransform, frameDamage, framePresentation, frameReady, frameFailed
};

static void sessionBufferSize(void* data, struct ext_image_copy_capture_session_v1* s, uint32_t w, uint32_t h)
{
    (void)data; (void)s;
    beginConstraints();
    proposedWidth = w;
    proposedHeight = h;
}

static void sessionShmFormat(void* data, struct ext_image_copy_capture_session_v1* s, uint32_t value)
{
    (void)data; (void)s;
    beginConstraints();
    // Prefer the format already in the recorded raw file when it is still
    // allowed; otherwise select a fresh supported format for this batch.
    if (vela_capture_supported_format(value) &&
        (!hasProposedFormat || (width && value == format))) {
        hasProposedFormat = true;
        proposedFormat = value;
    }
}

static void sessionDmabufDevice(void* data, struct ext_image_copy_capture_session_v1* s, struct wl_array* device)
{
    (void)data; (void)s; (void)device;
    beginConstraints();
}

static void sessionDmabufFormat(void* data, struct ext_image_copy_capture_session_v1* s, uint32_t value,
    struct wl_array* modifiers)
{
    (void)data; (void)s; (void)value; (void)modifiers;
    beginConstraints();
}

static void sessionDone(void* data, struct ext_image_copy_capture_session_v1* s)
{
    (void)data; (void)s;
    if (!constraintsCollecting) return;
    constraintsCollecting = false;
    if (!proposedWidth || !proposedHeight || !hasProposedFormat) {
        fprintf(stderr, "vela-screen-record: no supported SHM format in capture constraints\n");
        failed = true;
        stopping = 1;
        return;
    }
    if (width && (proposedWidth != width || proposedHeight != height || proposedFormat != format)) {
        reallocate = true;
    }
    if (frame) {
        // A BUFFER_CONSTRAINTS failure can arrive after session.done even if
        // size and format appear unchanged. Keep the completion handshake.
        constraintsWaiting = true;
    } else if (!stopping && (!width || reallocate)) {
        resume();
    }
}

static void sessionStopped(void* data, struct ext_image_copy_capture_session_v1* s)
{
    (void)data; (void)s;
    failed = !stopping;
    stopping = 1;
}

static const struct ext_image_copy_capture_session_v1_listener sessionListener = {
    sessionBufferSize, sessionShmFormat, sessionDmabufDevice, sessionDmabufFormat, sessionDone, sessionStopped
};

static void outputGeometry(void* data, struct wl_output* o, int32_t x, int32_t y,
    int32_t physicalW, int32_t physicalH, int32_t subpixel, const char* make, const char* model,
    int32_t outputTransform)
{
    (void)data; (void)o; (void)x; (void)y; (void)physicalW; (void)physicalH;
    (void)subpixel; (void)make; (void)model; (void)outputTransform;
}

static void outputMode(void* data, struct wl_output* o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
    (void)data; (void)o; (void)flags; (void)w; (void)h; (void)refresh;
}

static void outputDone(void* data, struct wl_output* o) { (void)data; (void)o; }
static void outputScale(void* data, struct wl_output* o, int32_t scale)
{ (void)data; (void)o; (void)scale; }
static void outputName(void* data, struct wl_output* o, const char* name)
{
    (void)o;
    struct recording_output* entry = data;
    free(entry->name);
    entry->name = strdup(name);
}
static void outputDescription(void* data, struct wl_output* o, const char* description)
{ (void)data; (void)o; (void)description; }

static const struct wl_output_listener outputListener = {
    outputGeometry, outputMode, outputDone, outputScale, outputName, outputDescription
};

static void registryGlobal(void* data, struct wl_registry* registry, uint32_t name,
    const char* interface, uint32_t version)
{
    (void)data;
    if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        struct recording_output* entry = calloc(1, sizeof(*entry));
        if (!entry) {
            failed = true;
            stopping = 1;
            return;
        }
        entry->wl = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
        wl_output_add_listener(entry->wl, &outputListener, entry);
        // Preserve the registry order for the legacy numeric selector.
        struct recording_output** tail = &outputs;
        while (*tail) tail = &(*tail)->next;
        *tail = entry;
    } else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
        sources = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
        manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    }
}

static void registryRemove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registryListener = { registryGlobal, registryRemove };

static bool selectOutput(void)
{
    const char* wanted = getenv("VELA_SCREEN_RECORD_OUTPUT");
    if (!wanted || !*wanted) wanted = "0";
    char* end;
    errno = 0;
    long index = strtol(wanted, &end, 10);
    bool byIndex = !errno && end != wanted && !*end && index >= 0;
    long seen = 0;
    for (struct recording_output* entry = outputs; entry; entry = entry->next, ++seen) {
        if ((byIndex && seen == index) ||
            (!byIndex && entry->name && !strcmp(entry->name, wanted))) {
            output = entry->wl;
            return true;
        }
    }
    fprintf(stderr, "vela-screen-record: output '%s' not found (use its wl_output name)\n", wanted);
    return false;
}

/** Open every directory component without following symlinks, including
 * parent directories; then create a fresh private file (never truncate an
 * existing path, including a hard link). */
static int privateDirectory(const char* directory)
{
    char* path = strdup(directory);
    if (!path) return -1;
    int directoryFd = open(directory[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directoryFd < 0) {
        free(path);
        return -1;
    }
    char* save = NULL;
    for (char* part = strtok_r(path, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            errno = EINVAL;
            close(directoryFd);
            free(path);
            return -1;
        }
        int next = openat(directoryFd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            close(directoryFd);
            free(path);
            return -1;
        }
        close(directoryFd);
        directoryFd = next;
    }
    free(path);
    return directoryFd;
}

static FILE* openPrivate(const char* directory, const char* name)
{
    int directoryFd = privateDirectory(directory);
    if (directoryFd < 0) {
        perror(directory);
        return NULL;
    }
    int fd = openat(directoryFd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    int savedError = errno;
    close(directoryFd);
    errno = savedError;
    if (fd < 0) {
        perror(name);
        return NULL;
    }
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        perror(name);
        close(fd);
    }
    return file;
}

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 6) {
        fprintf(stderr, "Usage: %s DIRECTORY [X Y WIDTH HEIGHT]\n", argv[0]);
        return 2;
    }
    if (argc == 6) {
        regionX = atoi(argv[2]);
        regionY = atoi(argv[3]);
        regionW = atoi(argv[4]);
        regionH = atoi(argv[5]);
        if (regionW <= 0 || regionH <= 0) {
            fprintf(stderr, "vela-screen-record: empty region\n");
            return 2;
        }
    }
    const char* factor = getenv("VELA_SCREEN_RECORD_DOWNSCALE");
    if (factor) {
        downscale = atoi(factor);
        if (downscale < 1 || downscale > 16) {
            fprintf(stderr, "vela-screen-record: VELA_SCREEN_RECORD_DOWNSCALE goes from 1 to 16\n");
            return 2;
        }
    }
    buffers[0].fd = buffers[1].fd = -1;

    // No SA_RESTART: a signal interrupts poll() below.
    struct sigaction action = { .sa_handler = stop };
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "vela-screen-record: no Wayland session (WAYLAND_DISPLAY)\n");
        return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registryListener, NULL);
    // The second roundtrip receives wl_output.name events from the objects
    // bound during the first one.
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0 ||
        failed || !selectOutput() || !manager || !shm || !output || !sources) {
        fprintf(stderr, "vela-screen-record: the compositor doesn't offer ext-image-copy-capture\n");
        return 1;
    }
    raw = openPrivate(argv[1], "frames.raw");
    times = raw ? openPrivate(argv[1], "frames.txt") : NULL;
    if (!raw || !times) {
        return 1;
    }

    struct ext_image_capture_source_v1* source =
        ext_output_image_capture_source_manager_v1_create_source(sources, output);
    session = ext_image_copy_capture_manager_v1_create_session(manager, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &sessionListener, NULL);

    // Keep termination signals pending between stopping checks. ppoll()
    // temporarily restores the previous mask atomically with the wait, so a
    // SIGINT/SIGTERM cannot land just before poll() and strand a still scene.
    sigset_t blocked, previousMask, waitingMask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &blocked, &previousMask) < 0) {
        perror("vela-screen-record: sigprocmask");
        return 1;
    }
    waitingMask = previousMask;
    sigdelset(&waitingMask, SIGINT);
    sigdelset(&waitingMask, SIGTERM);

    const int fd = wl_display_get_fd(display);
    while (!stopping) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                failed = true;
                stopping = 1;
                break;
            }
        }
        if (stopping) break;
        int flush = wl_display_flush(display);
        if (flush < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            perror("vela-screen-record: Wayland flush");
            failed = true;
            break;
        }
        // When the write buffer is full, wake on POLLOUT to retry flush.
        struct pollfd ready = { fd, (short)(POLLIN | (flush < 0 ? POLLOUT : 0)), 0 };
        if (ppoll(&ready, 1, NULL, &waitingMask) < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR) continue;
            perror("vela-screen-record");
            failed = true;
            break;
        }
        if (ready.revents & POLLIN) {
            if (wl_display_read_events(display) < 0 || wl_display_dispatch_pending(display) < 0) {
                fprintf(stderr, "vela-screen-record: Wayland connection lost\n");
                failed = true;
                break;
            }
        } else {
            wl_display_cancel_read(display);
        }
        if (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "vela-screen-record: Wayland socket closed\n");
            failed = true;
            break;
        }
    }

    // Restore the caller's mask now that the wait loop has finished.
    if (sigprocmask(SIG_SETMASK, &previousMask, NULL) < 0) {
        perror("vela-screen-record: restore signal mask");
        failed = true;
    }
    if (frame) ext_image_copy_capture_frame_v1_destroy(frame);
    ext_image_copy_capture_session_v1_destroy(session);
    ext_image_capture_source_v1_destroy(source);
    freeBuffers();
    wl_display_flush(display);
    wl_display_disconnect(display);
    free(row);
    const bool written = fclose(raw) == 0 && fclose(times) == 0;
    if (!written) {
        perror("vela-screen-record");
    }
    fprintf(stderr, "vela-screen-record: %lu frames\n", frames);
    return failed || !written || frames == 0 ? 1 : 0;
}
