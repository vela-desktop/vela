// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Neutral SHM animation and presentation-time diagnostic. JSONL records are
// protocol evidence, not optical latency, input latency or a desktop ranking.
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

enum { WIDTH = 640, HEIGHT = 360, BUFFERS = 3 };
static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm;
static struct wp_presentation* presentation;
static struct wl_surface* surface;
static struct wl_callback* frameCallback;
static uint32_t requested = 120, delayMs, timeoutSeconds = 30;
static uint32_t committed, presented, discarded, outstanding;
static clockid_t presentationClock;
static bool haveClock, configured, frameReady = true;
static uint32_t inputRequested, inputReceived, inputDeliveredMs;
static uint64_t inputReceiptNs;
static bool keyboardFocused, inputReady;
static bool holdAfterSummary;
static volatile sig_atomic_t running = 1;

struct Buffer {
    struct wl_buffer* object;
    uint32_t* pixels;
    bool busy;
};
static struct Buffer buffers[BUFFERS];

struct Feedback {
    uint32_t frame;
    uint64_t commitClockNs;
};

static void fail(const char* message)
{
    fprintf(stderr, "vela-bench-probe: %s\n", message);
    exit(1);
}

static uint64_t now(clockid_t clock)
{
    struct timespec value;
    if (clock_gettime(clock, &value) < 0) {
        fail("cannot query the advertised clock");
    }
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static void stop(int signum)
{
    running = 0;
}

static void deadlineExpired(int signum)
{
    // A stalled server can also block a startup round trip. Use only
    // async-signal-safe operations here; a partial stream stays incomplete.
    static const char message[] = "vela-bench-probe: deadline expired\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void submit(void);

static void announceInputReady(void)
{
    if (inputRequested && keyboardFocused && presented && !inputReady) {
        inputReady = true;
        printf("{\"event\":\"input-ready\",\"monotonic_ns\":%" PRIu64 "}\n", now(CLOCK_MONOTONIC));
    }
}

static void releaseBuffer(void* data, struct wl_buffer* object)
{
    struct Buffer* buffer = data;
    buffer->busy = false;
    submit();
}
static const struct wl_buffer_listener bufferListener = { releaseBuffer };

static void frameDone(void* data, struct wl_callback* callback, uint32_t time)
{
    wl_callback_destroy(callback);
    frameCallback = NULL;
    frameReady = true;
    submit();
}
static const struct wl_callback_listener frameListener = { frameDone };

static void syncOutput(void* data, struct wp_presentation_feedback* feedback, struct wl_output* output) { }

static void didPresent(void* data, struct wp_presentation_feedback* feedback,
    uint32_t secHi, uint32_t secLo, uint32_t nsec, uint32_t refresh,
    uint32_t seqHi, uint32_t seqLo, uint32_t flags)
{
    struct Feedback* record = data;
    const uint64_t seconds = (uint64_t)secHi << 32 | secLo;
    if (nsec >= 1000000000u || seconds > (UINT64_MAX - nsec) / 1000000000ull) {
        fail("invalid presentation timestamp");
    }
    const uint64_t timestamp = seconds * 1000000000ull + nsec;
    printf("{\"event\":\"presented\",\"frame\":%u,\"presentation_ns\":%" PRIu64
           ",\"commit_clock_ns\":%" PRIu64 ",\"received_monotonic_ns\":%" PRIu64
           ",\"refresh_ns\":%u,\"sequence\":%" PRIu64 ",\"flags\":%u}\n",
        record->frame, timestamp, record->commitClockNs, now(CLOCK_MONOTONIC),
        refresh, (uint64_t)seqHi << 32 | seqLo, flags);
    ++presented;
    announceInputReady();
    --outstanding;
    wp_presentation_feedback_destroy(feedback);
    free(record);
}

static void didDiscard(void* data, struct wp_presentation_feedback* feedback)
{
    struct Feedback* record = data;
    printf("{\"event\":\"discarded\",\"frame\":%u,\"received_monotonic_ns\":%" PRIu64 "}\n",
        record->frame, now(CLOCK_MONOTONIC));
    ++discarded;
    --outstanding;
    wp_presentation_feedback_destroy(feedback);
    free(record);
}
static const struct wp_presentation_feedback_listener feedbackListener = { syncOutput, didPresent, didDiscard };

static void submit(void)
{
    if (!configured || !haveClock || !frameReady || !running || committed == requested ||
        (inputRequested && committed > inputReceived)) {
        return;
    }
    struct Buffer* buffer = NULL;
    for (int i = 0; i < BUFFERS; ++i) {
        if (!buffers[i].busy) {
            buffer = &buffers[i];
            break;
        }
    }
    if (!buffer) {
        return;
    }
    uint64_t delayBegin = now(CLOCK_MONOTONIC);
    struct timespec pause = { .tv_sec = delayMs / 1000, .tv_nsec = (delayMs % 1000) * 1000000l };
    while (running && nanosleep(&pause, &pause) < 0 && errno == EINTR) { }
    if (!running) {
        return;
    }
    const uint64_t actualDelay = now(CLOCK_MONOTONIC) - delayBegin;
    const uint32_t id = committed + 1;
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            // A moving bar and an explicit 32-bit frame ID for future capture
            // calibration. Pixels are not claimed to be physical-size pixels.
            // Inset the marker so default rounded window corners cannot
            // clip its anchors. No desktop effects are disabled for it.
            const bool marker = y >= 16 && y < 48;
            const bool bit = marker && x >= 48 && x < 592 && ((id >> ((x - 48) / 17)) & 1u);
            const bool bar = y >= 48 && (x + WIDTH - (int)(id * 7 % WIDTH)) % WIDTH < 20;
            buffer->pixels[y * WIDTH + x] = bit ? 0xffffffffu : bar ? 0xff20b8d0u : 0xff202830u;
            if (marker) {
                if (x >= 16 && x < 32) buffer->pixels[y * WIDTH + x] = 0xffd020d0u;
                else if (x >= 32 && x < 48) buffer->pixels[y * WIDTH + x] = 0xff20d020u;
                else if (x >= 592 && x < 608) buffer->pixels[y * WIDTH + x] = 0xff2040e0u;
                else if (x >= 608 && x < 624) buffer->pixels[y * WIDTH + x] = 0xffe08020u;
            }
        }
    }
    struct Feedback* record = calloc(1, sizeof(*record));
    if (!record) {
        fail("out of memory");
    }
    record->frame = id;
    struct wp_presentation_feedback* feedback = wp_presentation_feedback(presentation, surface);
    wp_presentation_feedback_add_listener(feedback, &feedbackListener, record);
    frameCallback = wl_surface_frame(surface);
    wl_callback_add_listener(frameCallback, &frameListener, NULL);
    wl_surface_attach(surface, buffer->object, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);
    record->commitClockNs = now(presentationClock);
    printf("{\"event\":\"commit\",\"frame\":%u,\"commit_clock_ns\":%" PRIu64
           ",\"monotonic_ns\":%" PRIu64 ",\"delay_elapsed_ns\":%" PRIu64
           ",\"input_id\":%u,\"input_received_monotonic_ns\":%" PRIu64 ",\"input_delivered_ms\":%u}\n",
        id, record->commitClockNs, now(CLOCK_MONOTONIC), actualDelay, inputReceived, inputReceiptNs, inputDeliveredMs);
    wl_surface_commit(surface);
    buffer->busy = true;
    frameReady = false;
    ++committed;
    ++outstanding;
}

static void clockId(void* data, struct wp_presentation* object, uint32_t id)
{
    presentationClock = (clockid_t)id;
    (void)now(presentationClock);
    haveClock = true;
    printf("{\"event\":\"clock\",\"presentation_clock_id\":%u,\"monotonic_clock_id\":%d}\n", id, CLOCK_MONOTONIC);
}
static const struct wp_presentation_listener presentationListener = { clockId };

static void keymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int fd, uint32_t size) { close(fd); }
static void keyboardEnter(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* entered, struct wl_array* keys)
{
    keyboardFocused = entered == surface;
    announceInputReady();
}
static void keyboardLeave(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* left)
{
    keyboardFocused = false;
}
static void keyboardKey(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    if (!inputRequested || key != KEY_F24 || state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (!inputReady || !keyboardFocused || inputReceived == inputRequested) fail("unexpected F24 action");
    inputReceiptNs = now(CLOCK_MONOTONIC);
    inputDeliveredMs = time;
    ++inputReceived;
    printf("{\"event\":\"input\",\"action_id\":%u,\"received_monotonic_ns\":%" PRIu64 ",\"delivered_ms\":%u}\n",
        inputReceived, inputReceiptNs, time);
    submit();
}
static void modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) { }
static const struct wl_keyboard_listener keyboardListener = {
    .keymap = keymap, .enter = keyboardEnter, .leave = keyboardLeave, .key = keyboardKey, .modifiers = modifiers,
};
static struct wl_keyboard* keyboard;
static void capabilities(void* data, struct wl_seat* seat, uint32_t caps)
{
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard, &keyboardListener, NULL);
    }
}
static const struct wl_seat_listener seatListener = { .capabilities = capabilities };

static void ping(void* data, struct xdg_wm_base* base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wmListener = { ping };

static void global(void* data, struct wl_registry* registry, uint32_t id, const char* interface, uint32_t version)
{
    if (!strcmp(interface, "wl_compositor") && version >= 4) {
        compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    } else if (!strcmp(interface, "wl_shm")) {
        shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    } else if (!strcmp(interface, "xdg_wm_base")) {
        wm = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm, &wmListener, NULL);
    } else if (!strcmp(interface, "wp_presentation")) {
        presentation = wl_registry_bind(registry, id, &wp_presentation_interface, 1);
        wp_presentation_add_listener(presentation, &presentationListener, NULL);
    } else if (inputRequested && !strcmp(interface, "wl_seat")) {
        struct wl_seat* seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seatListener, NULL);
    }
}
static void globalRemove(void* data, struct wl_registry* registry, uint32_t id) { }
static const struct wl_registry_listener registryListener = { global, globalRemove };

static void configure(void* data, struct xdg_surface* xdg, uint32_t serial)
{
    xdg_surface_ack_configure(xdg, serial);
    configured = true;
    submit();
}
static const struct xdg_surface_listener surfaceListener = { configure };
static void toplevelConfigure(void* data, struct xdg_toplevel* top, int32_t width, int32_t height, struct wl_array* states) { }
static void toplevelClose(void* data, struct xdg_toplevel* top) { running = 0; }
static const struct xdg_toplevel_listener toplevelListener = {
    .configure = toplevelConfigure,
    .close = toplevelClose,
};

static uint32_t argument(const char* value, uint32_t minimum, uint32_t maximum)
{
    char* end;
    errno = 0;
    unsigned long number = strtoul(value, &end, 10);
    if (errno || !*value || *end || number < minimum || number > maximum) {
        fail("argument outside supported range");
    }
    return (uint32_t)number;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--hold")) {
            holdAfterSummary = true;
            continue;
        }
        if (!strcmp(argv[i], "--help")) {
            puts("Usage: vela-bench-probe [--frames 1..10000] [--inputs 1..1000 [--hold]] [--delay-ms 0..1000] [--timeout 1..3600]\n"
                 "JSONL presentation diagnostic; --inputs observes F24 press to client submission/feedback.\n"
                 "--hold keeps the last input frame until stdin closes. No optical or whole-desktop latency claim.");
            return 0;
        }
        if (i + 1 >= argc) {
            fail("option requires a value; see --help");
        }
        const char* option = argv[i];
        const char* value = argv[++i];
        if (!strcmp(option, "--frames")) {
            requested = argument(value, 1, 10000);
        } else if (!strcmp(option, "--inputs")) {
            inputRequested = argument(value, 1, 1000);
        } else if (!strcmp(option, "--delay-ms")) {
            delayMs = argument(value, 0, 1000);
        } else if (!strcmp(option, "--timeout")) {
            timeoutSeconds = argument(value, 1, 3600);
        } else {
            fail("unknown option; see --help");
        }
    }
    if (inputRequested) {
        requested = inputRequested + 1; // One explicitly excluded warm-up frame.
    }
    if (holdAfterSummary && !inputRequested) fail("--hold is only available for input calibration");
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    // The deadline also bounds startup round trips if a server stops replying.
    signal(SIGALRM, deadlineExpired);
    alarm(timeoutSeconds);
    display = wl_display_connect(NULL);
    if (!display) {
        fail("cannot connect to Wayland");
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, NULL);
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) {
        fail("Wayland registry exchange failed");
    }
    if (!compositor || !shm || !wm || !presentation || !haveClock) {
        fail("requires wl_compositor v4, wl_shm, xdg-shell and presentation-time");
    }
    printf("{\"event\":\"probe\",\"schema\":1,\"method\":\"%s\","
           "\"scope\":\"%s\",\"display_protocol\":\"wayland\",\"window_protocol\":\"xdg-shell\","
           "\"buffer_backend\":\"wl_shm\",\"buffer_width\":%u,\"buffer_height\":%u,\"frames\":%u,\"delay_requested_ms\":%u}\n",
           inputRequested ? "wayland-input-presentation-v1" : "wayland-presentation-v1",
           inputRequested ? "software-input-to-reported-presentation" : "diagnostic-client-only", WIDTH, HEIGHT, requested, delayMs);
    for (int i = 0; i < BUFFERS; ++i) {
        int fd = memfd_create("vela-bench-probe", MFD_CLOEXEC);
        const size_t length = WIDTH * HEIGHT * 4;
        if (fd < 0 || ftruncate(fd, (off_t)length) < 0) {
            fail("cannot allocate SHM buffer");
        }
        buffers[i].pixels = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (buffers[i].pixels == MAP_FAILED) {
            fail("cannot map SHM buffer");
        }
        struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, (int32_t)length);
        buffers[i].object = wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, WIDTH * 4, WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(buffers[i].object, &bufferListener, &buffers[i]);
        wl_shm_pool_destroy(pool);
        close(fd);
    }
    surface = wl_compositor_create_surface(compositor);
    struct xdg_surface* xdg = xdg_wm_base_get_xdg_surface(wm, surface);
    xdg_surface_add_listener(xdg, &surfaceListener, NULL);
    struct xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(toplevel, &toplevelListener, NULL);
    xdg_toplevel_set_title(toplevel, "Vela benchmark - native Wayland diagnostic");
    xdg_toplevel_set_app_id(toplevel, "org.vela.benchmark.probe");
    xdg_toplevel_set_min_size(toplevel, WIDTH, HEIGHT);
    xdg_toplevel_set_max_size(toplevel, WIDTH, HEIGHT);
    wl_surface_commit(surface);
    const uint64_t deadline = now(CLOCK_MONOTONIC) + (uint64_t)timeoutSeconds * 1000000000ull;
    bool connectionError = false;
    while (running && (committed < requested || outstanding)) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                connectionError = true;
                break;
            }
        }
        if (connectionError) {
            break;
        }
        const int flush = wl_display_flush(display);
        if (flush < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            connectionError = true;
            break;
        }
        struct pollfd descriptor = { .fd = wl_display_get_fd(display), .events = POLLIN | (flush < 0 ? POLLOUT : 0) };
        int result = poll(&descriptor, 1, 100);
        if (result > 0 && descriptor.revents & POLLIN) {
            if (wl_display_read_events(display) < 0 || wl_display_dispatch_pending(display) < 0) {
                connectionError = true;
                break;
            }
        } else {
            wl_display_cancel_read(display);
            if ((result < 0 && errno != EINTR) || (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL))) {
                connectionError = true;
                break;
            }
        }
        if (now(CLOCK_MONOTONIC) >= deadline) {
            break;
        }
    }
    const bool complete = !connectionError && committed == requested && outstanding == 0 && presented > 0;
    printf("{\"event\":\"summary\",\"complete\":%s,\"committed\":%u,\"presented\":%u,\"discarded\":%u}\n",
        complete ? "true" : "false", committed, presented, discarded);
    if (!complete) {
        fprintf(stderr, "vela-bench-probe: incomplete feedback (timeout, interruption, closure or connection failure)\n");
    }
    // Keep the last fixture visible while capture finishes. Continue to
    // answer compositor pings; EOF on the private stdin pipe releases it.
    while (complete && holdAfterSummary && running) {
        if (wl_display_dispatch_pending(display) < 0) break;
        struct pollfd fds[] = {{.fd = STDIN_FILENO, .events = POLLIN},
                               {.fd = wl_display_get_fd(display), .events = POLLIN}};
        if (wl_display_flush(display) < 0 && errno != EAGAIN) break;
        int result = poll(fds, 2, 100);
        if (result < 0 && errno != EINTR) break;
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) break;
        if ((fds[1].revents & POLLIN) && wl_display_dispatch(display) < 0) break;
        if (fds[1].revents & (POLLHUP | POLLERR)) break;
    }
    wl_display_disconnect(display);
    return complete ? 0 : 1;
}
