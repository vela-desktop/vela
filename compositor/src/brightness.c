// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "brightness.h"

#include "brightness_model.h"
#include "buffer.h"
#include "command.h"
#include "config.h"
#include "listen.h"
#include "output.h"
#include "process.h"
#include "server.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#define WATCHDOG_MS 5000 // a transaction busy this long is a hung transfer (§6)
#define STOP_KILL_MS 5000 // a helper told to go and still there is killed
#define QUICK_CRASH_S 5.0 // an exit this soon after starting is a quick crash
#define QUICK_CRASHES 3 // ... and after this many in a row it isn't relaunched
#define HELPER_LINE_MAX 128 // the helper's VELA_LINE_MAX

// One vela-brightness process. It outlives its output when the output goes
// away: until it has exited, no other one starts for its connector.
struct helper {
    struct wl_list link; // vela_brightness.helpers
    struct vela_brightness *brightness;
    struct vela_brightness_output *owner; // NULL: its output is gone, waiting for it to exit
    char connector[96];
    struct vela_child child;
    int fd; // our end of the socketpair; -1 once closed
    struct wl_event_source *source;
    struct wl_event_source *timer; // the watchdog while busy; after a stop, the SIGKILL
    char line[HELPER_LINE_MAX + 1];
    size_t length;
    bool overlong;
    bool hung; // killed by the watchdog
    // What it was told (the helper starts powered, active, without live sync).
    bool power, session, live;
};

struct vela_brightness_output {
    struct wl_list link; // vela_brightness.outputs
    struct vela_brightness *brightness;
    struct vela_output *output;
    char connector[96]; // what the helper drives: "card1-DP-3", or the output's name
    struct vela_brightness_model model;
    struct helper *helper; // NULL: none running
    // Not started again (watchdog, crash loop) until the output is plugged in
    // again, wakes up, the session becomes active again, or Settings probes.
    bool held;
    int quick_crashes;
    bool was_powered;
    struct wl_listener commit;
};

struct vela_brightness {
    struct vela_server *server;
    bool enabled;
    bool fake;
    char command[PATH_MAX + 2];
    struct wl_list outputs; // vela_brightness_output.link
    struct wl_list helpers; // helper.link
    bool live;
    bool session_active;
    struct wl_listener session_active_changed;
    struct wl_event_source *sync;
    char *published; // the last JSON line sent to watchers
};

static void schedule_sync(struct vela_brightness *brightness);

// --------------------------------------------------------------- publish --

void vela_brightness_json(struct vela_server *server, struct vela_buffer *json)
{
    vela_buffer_append(json, "{\"outputs\":[");
    bool first = true;
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        if (!output->brightness || !server->brightness->enabled) {
            continue;
        }
        if (!first) {
            vela_buffer_append(json, ",");
        }
        vela_brightness_model_json(&output->brightness->model, output->wlr->name, json);
        first = false;
    }
    vela_buffer_append(json, "]}");
}

// Watchers get the whole state again when what they would see changed.
static void publish(struct vela_brightness *brightness)
{
    struct vela_buffer json = { 0 };
    vela_brightness_json(brightness->server, &json);
    const char *text = vela_buffer_text(&json);
    if (!brightness->published || strcmp(brightness->published, text) != 0) {
        free(brightness->published);
        brightness->published = strdup(text);
        vela_commands_notify(brightness->server, "brightness");
    }
    vela_buffer_finish(&json);
}

// ---------------------------------------------------------------- helpers --

static void close_socket(struct helper *helper)
{
    if (helper->source) {
        wl_event_source_remove(helper->source);
        helper->source = NULL;
    }
    if (helper->fd >= 0) {
        close(helper->fd);
        helper->fd = -1;
    }
}

static void helper_free(struct helper *helper)
{
    close_socket(helper);
    if (helper->timer) {
        wl_event_source_remove(helper->timer);
    }
    wl_list_remove(&helper->link);
    free(helper);
}

static void fail(struct vela_brightness_output *output)
{
    vela_brightness_model_fail(&output->model);
    publish(output->brightness);
}

static void kill_helper(struct helper *helper, int signal)
{
    if (helper->child.pid > 0) {
        kill(helper->child.pid, signal);
    }
}

// The helper goes away with its output; no line of its is read any more.
static void stop_helper(struct vela_brightness_output *output)
{
    struct helper *helper = output->helper;
    if (!helper) {
        return;
    }
    output->helper = NULL;
    helper->owner = NULL;
    close_socket(helper);
    kill_helper(helper, SIGTERM);
    wl_event_source_timer_update(helper->timer, STOP_KILL_MS);
}

static void send_line(struct helper *helper, const char *line)
{
    if (helper->fd < 0 || !*line) {
        return;
    }
    size_t length = strlen(line);
    ssize_t done = send(helper->fd, line, length, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (done == (ssize_t)length) {
        return;
    }
    // Lines are a few bytes: a socket that can't take one belongs to a helper
    // that stopped reading long ago.
    wlr_log(WLR_ERROR, "Brightness %s: the helper doesn't read its lines: stopping it", helper->connector);
    helper->hung = true;
    close_socket(helper);
    kill_helper(helper, SIGKILL);
    if (helper->owner) {
        fail(helper->owner);
    }
}

static void log_state(struct vela_brightness_output *output, const char *before)
{
    const char *now = vela_brightness_model_state_name(&output->model);
    if (strcmp(before, now) == 0) {
        return;
    }
    if (output->model.known) {
        wlr_log(WLR_INFO, "Brightness %s: %s (%u/%u)", output->output->wlr->name, now, output->model.confirmed,
            output->model.max);
    } else {
        wlr_log(WLR_INFO, "Brightness %s: %s", output->output->wlr->name, now);
    }
}

static void helper_line(struct helper *helper, const char *line)
{
    struct vela_brightness_output *output = helper->owner;
    if (!output) {
        return;
    }
    const char *before = vela_brightness_model_state_name(&output->model);
    switch (vela_brightness_model_line(&output->model, line)) {
    case VELA_BRIGHTNESS_LINE_INVALID:
        wlr_log(WLR_ERROR, "Brightness %s: unknown line \"%s\"", helper->connector, line);
        break;
    case VELA_BRIGHTNESS_LINE_BUSY:
        wl_event_source_timer_update(helper->timer, WATCHDOG_MS);
        break;
    case VELA_BRIGHTNESS_LINE_IDLE:
        wl_event_source_timer_update(helper->timer, 0);
        break;
    case VELA_BRIGHTNESS_LINE_UNCHANGED:
        break;
    case VELA_BRIGHTNESS_LINE_CHANGED:
        log_state(output, before);
        publish(output->brightness);
        break;
    }
}

static int handle_readable(int fd, uint32_t mask, void *data)
{
    struct helper *helper = data;
    char bytes[512];
    ssize_t n;
    while (helper->fd >= 0 && (n = read(helper->fd, bytes, sizeof(bytes))) > 0) {
        for (ssize_t i = 0; i < n && helper->fd >= 0; ++i) {
            char c = bytes[i];
            if (c == '\n') {
                if (!helper->overlong) {
                    helper->line[helper->length] = '\0';
                    helper_line(helper, helper->line);
                }
                helper->length = 0;
                helper->overlong = false;
            } else if (helper->length == HELPER_LINE_MAX) {
                helper->overlong = true;
            } else if (!helper->overlong) {
                helper->line[helper->length++] = c;
            }
        }
    }
    if (helper->fd >= 0 && (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))) {
        close_socket(helper); // it is going: its exit comes through the pidfd
    }
    return 0;
}

static int handle_timer(void *data)
{
    struct helper *helper = data;
    if (!helper->owner) {
        wlr_log(WLR_ERROR, "Brightness %s: the helper didn't go away: killing it", helper->connector);
        kill_helper(helper, SIGKILL);
        return 0;
    }
    // A transfer that hangs (§6): this output only; the others don't notice.
    wlr_log(WLR_ERROR, "Brightness %s: a transaction took more than %d s: killing the helper", helper->connector,
        WATCHDOG_MS / 1000);
    helper->hung = true;
    close_socket(helper);
    kill_helper(helper, SIGKILL);
    fail(helper->owner);
    return 0;
}

static void helper_exited(struct vela_child *child, int status)
{
    struct helper *helper = wl_container_of(child, helper, child);
    struct vela_brightness_output *output = helper->owner;
    double uptime = (vela_now_ns() - child->started_ns) / 1e9;
    bool hung = helper->hung;
    struct vela_brightness *brightness = helper->brightness;
    helper_free(helper);
    // A new output on the same connector may be waiting for this one to go.
    schedule_sync(brightness);
    if (!output) {
        return;
    }
    output->helper = NULL;
    char reason[64];
    if (WIFSIGNALED(status)) {
        snprintf(reason, sizeof(reason), "signal %s", strsignal(WTERMSIG(status)));
    } else {
        snprintf(reason, sizeof(reason), "code %d", WEXITSTATUS(status));
    }
    output->quick_crashes = uptime < QUICK_CRASH_S ? output->quick_crashes + 1 : 0;
    if (hung) {
        output->held = true;
    } else if (output->quick_crashes >= QUICK_CRASHES) {
        wlr_log(WLR_ERROR, "Brightness %s: vela-brightness keeps exiting (%s): not restarting it", output->connector,
            reason);
        output->held = true;
        fail(output);
    } else {
        wlr_log(WLR_ERROR, "Brightness %s: vela-brightness exited (%s): restarting it", output->connector, reason);
    }
}

static bool connector_in_use(struct vela_brightness *brightness, const char *connector)
{
    struct helper *helper;
    wl_list_for_each (helper, &brightness->helpers, link) {
        if (strcmp(helper->connector, connector) == 0) {
            return true;
        }
    }
    return false;
}

static void start_helper(struct vela_brightness_output *output)
{
    struct vela_brightness *brightness = output->brightness;
    struct vela_server *server = brightness->server;
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
        wlr_log_errno(WLR_ERROR, "Brightness %s: socketpair", output->connector);
        output->held = true;
        fail(output);
        return;
    }
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    struct helper *helper = calloc(1, sizeof(*helper));
    helper->brightness = brightness;
    helper->fd = fds[0];
    helper->power = helper->session = true;
    snprintf(helper->connector, sizeof(helper->connector), "%s", output->connector);
    wl_list_insert(brightness->helpers.prev, &helper->link);
    helper->timer = wl_event_loop_add_timer(server->loop, handle_timer, helper);
    vela_child_init(&helper->child, "vela-brightness", SIGTERM, -1);
    helper->child.keep_fd = fds[1];
    helper->child.exited = helper_exited;

    char command[sizeof(brightness->command) + 192];
    bool ok = vela_format(command, sizeof(command), "VELA_BRIGHTNESS_FD=%d exec %s ddc '%s'", fds[1],
                  brightness->command, output->connector)
        && vela_child_start(&helper->child, server->loop, command);
    close(fds[1]);
    if (!ok) {
        // Not started, or started and not watched: without our end it exits.
        helper_free(helper);
        output->held = true;
        fail(output);
        return;
    }
    helper->owner = output;
    output->helper = helper;
    helper->source = wl_event_loop_add_fd(server->loop, helper->fd, WL_EVENT_READABLE, handle_readable, helper);
    vela_brightness_model_restart(&output->model);
    publish(brightness);
}

// ------------------------------------------------------------------- sync --

// What each output's helper should be doing, after anything changed: started
// or stopped, and told about power, the session and live sync.
static void sync_output(struct vela_brightness_output *output)
{
    struct vela_brightness *brightness = output->brightness;
    struct vela_output *out = output->output;
    bool powered = out->powered;
    if (powered && !output->was_powered) {
        output->held = false; // a wake-up
        output->quick_crashes = 0;
    }
    output->was_powered = powered;
    // Off for idle, the helper stays (and stays quiet); disabled in Settings,
    // it goes.
    output->model.off = !out->wlr->enabled || !powered;
    bool wanted = brightness->enabled && (out->wlr->enabled || !powered);
    if (!wanted) {
        if (output->helper) {
            stop_helper(output);
            vela_brightness_model_restart(&output->model);
        }
        return;
    }
    if (!output->helper && !output->held && !connector_in_use(brightness, output->connector)) {
        start_helper(output);
    }
    struct helper *helper = output->helper;
    if (!helper) {
        return;
    }
    if (helper->power != powered) {
        helper->power = powered;
        send_line(helper, powered ? "power on\n" : "power off\n");
    }
    if (helper->fd >= 0 && helper->session != brightness->session_active) {
        helper->session = brightness->session_active;
        send_line(helper, helper->session ? "session active\n" : "session inactive\n");
    }
    if (helper->fd >= 0 && helper->live != brightness->live) {
        helper->live = brightness->live;
        send_line(helper, helper->live ? "live on\n" : "live off\n");
    }
}

static void sync_all(void *data)
{
    struct vela_brightness *brightness = data;
    brightness->sync = NULL;
    struct vela_brightness_output *output;
    wl_list_for_each (output, &brightness->outputs, link) {
        sync_output(output);
    }
    publish(brightness);
}

static void schedule_sync(struct vela_brightness *brightness)
{
    if (!brightness->sync) {
        brightness->sync = wl_event_loop_add_idle(brightness->server->loop, sync_all, brightness);
    }
}

static void handle_commit(struct wl_listener *listener, void *data)
{
    struct vela_brightness_output *output = wl_container_of(listener, output, commit);
    const struct wlr_output_event_commit *event = data;
    if (event->state->committed & WLR_OUTPUT_STATE_ENABLED) {
        if (event->state->enabled) {
            // Back on (a wake-up, or Settings): worth another helper, even if
            // it was off for less than a turn of the loop.
            output->held = false;
            output->quick_crashes = 0;
        }
        schedule_sync(output->brightness); // after vela_output_set_powered has noted why
    }
}

static void handle_session_active(struct wl_listener *listener, void *data)
{
    struct vela_brightness *brightness = wl_container_of(listener, brightness, session_active_changed);
    bool active = brightness->server->session->active;
    if (active && !brightness->session_active) {
        struct vela_brightness_output *output;
        wl_list_for_each (output, &brightness->outputs, link) {
            output->held = false;
            output->quick_crashes = 0;
        }
    }
    brightness->session_active = active;
    schedule_sync(brightness);
}

// --------------------------------------------------------------- outputs --

// The DRM connector, as sysfs names it: "card1-DP-3".
static bool drm_connector(struct wlr_output *wlr, char *out, size_t size)
{
    int fd = wlr_backend_get_drm_fd(wlr->backend);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISCHR(info.st_mode)) {
        return false;
    }
    char link[64];
    char target[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/dev/char/%u:%u", major(info.st_rdev), minor(info.st_rdev));
    ssize_t length = readlink(link, target, sizeof(target) - 1);
    if (length <= 0) {
        return false;
    }
    target[length] = '\0';
    const char *card = strrchr(target, '/');
    card = card ? card + 1 : target;
    return strncmp(card, "card", 4) == 0 && vela_format(out, size, "%s-%s", card, wlr->name);
}

void vela_brightness_output_added(struct vela_output *output)
{
    struct vela_brightness *brightness = output->server->brightness;
    if (!brightness || output->brightness) {
        return;
    }
    struct wlr_output *wlr = output->wlr;
    bool drm = wlr_output_is_drm(wlr);
    if (!drm && !brightness->fake) {
        return; // nested or headless: no monitor of ours behind it
    }
    struct vela_brightness_output *entry = calloc(1, sizeof(*entry));
    entry->brightness = brightness;
    entry->output = output;
    entry->was_powered = output->powered;
    if (!drm || !drm_connector(wlr, entry->connector, sizeof(entry->connector))) {
        if (drm) {
            wlr_log(WLR_ERROR, "Brightness %s: can't find its DRM card", wlr->name);
        }
        snprintf(entry->connector, sizeof(entry->connector), "%s", wlr->name);
    }
    vela_brightness_model_init(&entry->model);
    vela_listen(&wlr->events.commit, &entry->commit, handle_commit);
    wl_list_insert(brightness->outputs.prev, &entry->link);
    output->brightness = entry;
    schedule_sync(brightness);
}

void vela_brightness_output_removed(struct vela_output *output)
{
    struct vela_brightness_output *entry = output->brightness;
    if (!entry) {
        return;
    }
    stop_helper(entry);
    wl_list_remove(&entry->commit.link);
    wl_list_remove(&entry->link);
    output->brightness = NULL;
    struct vela_brightness *brightness = entry->brightness;
    free(entry);
    publish(brightness);
}

static struct vela_brightness_output *find_output(struct vela_server *server, const char *name)
{
    struct vela_output *output = vela_output_named(server, name);
    return output ? output->brightness : NULL;
}

// ------------------------------------------------------------- commands --

bool vela_brightness_set(struct vela_server *server, const char *name, double value)
{
    struct vela_brightness_output *output = server->brightness ? find_output(server, name) : NULL;
    char line[64];
    if (!output || !vela_brightness_model_set(&output->model, value, line, sizeof(line))) {
        return false;
    }
    if (output->helper) {
        send_line(output->helper, line);
    }
    publish(output->brightness);
    return true;
}

void vela_brightness_probe(struct vela_server *server, const char *name)
{
    struct vela_brightness_output *output = server->brightness ? find_output(server, name) : NULL;
    if (!output) {
        return;
    }
    if (output->held) {
        // Settings is showing this output: worth another helper.
        output->held = false;
        output->quick_crashes = 0;
        schedule_sync(output->brightness);
    }
    if (output->helper) {
        send_line(output->helper, "probe\n");
    }
}

void vela_brightness_live(struct vela_server *server, bool on)
{
    struct vela_brightness *brightness = server->brightness;
    if (brightness && brightness->live != on) {
        brightness->live = on;
        schedule_sync(brightness);
    }
}

// ------------------------------------------------------------- lifetime --

void vela_brightness_load_settings(struct vela_server *server)
{
    struct vela_brightness *brightness = server->brightness;
    if (!brightness) {
        return;
    }
    bool was = brightness->enabled;
    brightness->enabled = false;
    const char *fake = getenv("VELA_BRIGHTNESS_FAKE");
    brightness->fake = fake && *fake;
    const char *custom = getenv("VELA_BRIGHTNESS");
    if (custom && *custom) {
        if (strcmp(custom, "0") != 0) {
            brightness->enabled = vela_format(brightness->command, sizeof(brightness->command), "%s", custom);
        }
    } else if (server->session || brightness->fake) {
        struct vela_config settings;
        vela_config_read(&settings);
        bool on = vela_config_flag(&settings, "brightness", true);
        vela_config_finish(&settings);
        if (!on) {
            wlr_log(WLR_INFO, "Brightness disabled in vela.conf");
        } else if (!vela_find_program("vela-brightness", "brightness", brightness->command,
                       sizeof(brightness->command))) {
            wlr_log(WLR_ERROR, "Brightness: vela-brightness not found");
        } else {
            brightness->enabled = true;
        }
    }
    if (brightness->enabled != was) {
        wlr_log(WLR_INFO, "Brightness: %s", brightness->enabled ? brightness->command : "off");
    }
    schedule_sync(brightness);
}

void vela_brightness_start(struct vela_server *server)
{
    struct vela_brightness *brightness = calloc(1, sizeof(*brightness));
    brightness->server = server;
    brightness->session_active = !server->session || server->session->active;
    wl_list_init(&brightness->outputs);
    wl_list_init(&brightness->helpers);
    wl_list_init(&brightness->session_active_changed.link);
    if (server->session) {
        vela_listen(&server->session->events.active, &brightness->session_active_changed, handle_session_active);
    }
    server->brightness = brightness;
    vela_brightness_load_settings(server);
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        vela_brightness_output_added(output);
    }
}

void vela_brightness_destroy(struct vela_server *server)
{
    struct vela_brightness *brightness = server->brightness;
    if (!brightness) {
        return;
    }
    // Without telling the watchers: the session is ending.
    struct vela_brightness_output *output, *next_output;
    wl_list_for_each_safe (output, next_output, &brightness->outputs, link) {
        stop_helper(output);
        wl_list_remove(&output->commit.link);
        wl_list_remove(&output->link);
        output->output->brightness = NULL;
        free(output);
    }
    struct helper *helper, *next;
    wl_list_for_each_safe (helper, next, &brightness->helpers, link) {
        kill_helper(helper, SIGTERM);
        vela_child_stop(&helper->child);
        helper_free(helper);
    }
    wl_list_remove(&brightness->session_active_changed.link);
    if (brightness->sync) {
        wl_event_source_remove(brightness->sync);
    }
    free(brightness->published);
    free(brightness);
    server->brightness = NULL;
}
