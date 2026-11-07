// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Searching and being discoverable (docs/bluetooth.md §6.6). Clients hold
// leases; while one is held, the daemon keeps one BlueZ discovery session
// of its own, suspended while an Add pairs. A discoverable lease uses
// BlueZ's Discoverable filter only when no other client was discovering,
// because BlueZ keeps the adapter visible until the last discovering client
// stops (rule 1). If the adapter is still visible two seconds after Vela's
// search ended, and wasn't before it, StillVisible says so (rule 3); only
// the user's "Stop being visible" writes Discoverable.

#include "daemon.h"

#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define ADAPTER1 "org.bluez.Adapter1"
#define VISIBLE_CHECK_US (2 * 1000000ULL)
#define STOP_RETRY_US (1000000ULL)

static void lease_free(struct vela_bt_lease *lease)
{
    sd_bus_track_unref(lease->owner);
    sd_event_source_disable_unref(lease->timer);
    free(lease);
}

static bool any_discoverable(const struct vela_bt_daemon *d)
{
    for (size_t i = 0; i < d->search.count; ++i) {
        if (d->search.leases[i]->discoverable) {
            return true;
        }
    }
    return false;
}

const char *vela_bt_search_mode(const struct vela_bt_daemon *d)
{
    if (d->search.count == 0) {
        return "";
    }
    if (!any_discoverable(d)) {
        return "plain";
    }
    return d->search.others ? "shared" : "visible";
}

static void set_still_visible(struct vela_bt_daemon *d, bool visible)
{
    if (d->search.still_visible != visible) {
        d->search.still_visible = visible;
        vela_bt_log("%s", visible ? "the adapter is still visible after Vela's search" : "the adapter is no longer visible");
        vela_bt_properties_changed(d);
    }
}

// ------------------------------------------------- the discovery itself --

// Vela's discovery: wanted while a lease is held, the session is in front,
// the adapter in use is on, no Add pairs, and no restart waits.
static bool wanted(struct vela_bt_daemon *d)
{
    const struct vela_bt_adapter *adapter = vela_bt_adapter_in_use(d);
    return d->search.count > 0 && vela_bt_active(d) && adapter && adapter->powered && !d->pairing
        && !d->search.restart_timer;
}

static void discovery_lost(struct vela_bt_daemon *d);

static int on_stop_retry(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_bt_daemon *d = data;
    d->search.stop_retry_timer = sd_event_source_disable_unref(d->search.stop_retry_timer);
    vela_bt_search_update(d);
    return 0;
}

// A failed StopDiscovery does not prove that BlueZ released our session.
static void retry_stop(struct vela_bt_daemon *d)
{
    if (!d->search.stop_retry_timer
        && sd_event_add_time_relative(d->event, &d->search.stop_retry_timer, CLOCK_MONOTONIC, STOP_RETRY_US, 0,
               on_stop_retry, d) < 0) {
        vela_bt_log("discovery retry timer: failed");
    }
}

static void stop_running(struct vela_bt_daemon *d);

static int on_stopped(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    struct vela_bt_search *s = &d->search;
    s->call = sd_bus_slot_unref(s->call);
    if (sd_bus_message_is_method_error(reply, NULL)) {
        vela_bt_log("StopDiscovery: %s", sd_bus_message_get_error(reply)->message);
        retry_stop(d);
        return 0;
    }
    s->running = false;
    s->saw_discovering = false;
    free(s->adapter);
    s->adapter = NULL;
    s->filtered = false;
    vela_bt_search_update(d);
    return 0;
}

static void stop_running(struct vela_bt_daemon *d)
{
    struct vela_bt_search *s = &d->search;
    if (s->stop_retry_timer) {
        return;
    }
    int r = vela_bt_call(d, &s->call, s->adapter, ADAPTER1, "StopDiscovery", VELA_BT_STEP_TIMEOUT_US,
        on_stopped, d, NULL);
    if (r < 0) {
        vela_bt_log("StopDiscovery call: %s", strerror(-r));
        retry_stop(d);
    }
}

static int on_started(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    struct vela_bt_search *s = &d->search;
    s->call = sd_bus_slot_unref(s->call);
    if (sd_bus_message_is_method_error(reply, NULL)) {
        vela_bt_log("StartDiscovery: %s", sd_bus_message_get_error(reply)->message);
        discovery_lost(d);
        return 0;
    }
    s->running = true;
    vela_bt_log("searching on %s%s", s->adapter, s->filtered ? ", discoverable" : "");
    vela_bt_search_update(d);
    return 0;
}

static int on_filter_set(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    struct vela_bt_search *s = &d->search;
    s->call = sd_bus_slot_unref(s->call);
    if (sd_bus_message_is_method_error(reply, NULL)) {
        vela_bt_log("SetDiscoveryFilter: %s", sd_bus_message_get_error(reply)->message);
        if (s->running) {
            // Changing a filter failed; the discovery is still ours.
            s->filtered = !s->filtered;
            retry_stop(d);
        } else {
            discovery_lost(d);
        }
        return 0;
    }
    if (s->running) {
        // A filter changed while discovering: BlueZ applies it at once.
        vela_bt_search_update(d);
        return 0;
    }
    const struct vela_bt_adapter *adapter = vela_bt_adapter_in_use(d);
    if (!wanted(d) || !adapter || !s->adapter || strcmp(s->adapter, adapter->path) != 0) {
        // A lease ended, pairing began or adapter changed during the call.
        vela_bt_search_update(d);
        return 0;
    }
    if (vela_bt_call(d, &s->call, s->adapter, ADAPTER1, "StartDiscovery", VELA_BT_STEP_TIMEOUT_US, on_started, d, NULL) < 0) {
        discovery_lost(d);
    }
    return 0;
}

static int set_filter(struct vela_bt_daemon *d, bool discoverable)
{
    struct vela_bt_search *s = &d->search;
    s->filtered = discoverable;
    // An empty filter takes off one set before by Vela.
    if (discoverable) {
        return vela_bt_call(d, &s->call, s->adapter, ADAPTER1, "SetDiscoveryFilter", VELA_BT_STEP_TIMEOUT_US,
            on_filter_set, d, "a{sv}", 1, "Discoverable", "b", 1);
    }
    return vela_bt_call(d, &s->call, s->adapter, ADAPTER1, "SetDiscoveryFilter", VELA_BT_STEP_TIMEOUT_US, on_filter_set,
        d, "a{sv}", 0);
}

static int on_restart(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_bt_daemon *d = data;
    d->search.restart_timer = sd_event_source_disable_unref(d->search.restart_timer);
    vela_bt_search_update(d);
    return 0;
}

// Vela's discovery stopped without Vela asking (BlueZ stopped it, or a
// call failed): it starts again, at most three times a minute.
static void discovery_lost(struct vela_bt_daemon *d)
{
    struct vela_bt_search *s = &d->search;
    s->running = false;
    s->saw_discovering = false;
    s->filtered = false;
    s->stop_retry_timer = sd_event_source_disable_unref(s->stop_retry_timer);
    free(s->adapter);
    s->adapter = NULL;
    if (s->count == 0) {
        return;
    }
    uint64_t now = vela_bt_now();
    size_t oldest = 0;
    for (size_t i = 1; i < 3; ++i) {
        if (s->restarts[i] < s->restarts[oldest]) {
            oldest = i;
        }
    }
    uint64_t allowed = s->restarts[oldest] ? s->restarts[oldest] + s->restart_window_us : 0;
    if (now < allowed) {
        vela_bt_log("discovery stopped again: waiting %llu s", (unsigned long long)((allowed - now) / 1000000ULL));
        if (!s->restart_timer
            && sd_event_add_time(d->event, &s->restart_timer, CLOCK_MONOTONIC, allowed, 0, on_restart, d) < 0) {
            vela_bt_log("discovery restart timer: failed");
        }
        s->restarts[oldest] = allowed;
        return;
    }
    vela_bt_log("discovery stopped: starting again");
    s->restarts[oldest] = now;
    vela_bt_search_update(d);
}

void vela_bt_search_update(struct vela_bt_daemon *d)
{
    struct vela_bt_search *s = &d->search;
    if (s->call) {
        return; // its reply looks again
    }
    // Its adapter went away with Vela's discovery.
    if (s->adapter && !vela_bt_adapter_find(d, s->adapter)) {
        s->running = false;
        s->saw_discovering = false;
        free(s->adapter);
        s->adapter = NULL;
    }
    int r = 0;
    if (wanted(d)) {
        const struct vela_bt_adapter *adapter = vela_bt_adapter_in_use(d);
        bool filter = any_discoverable(d) && !s->others;
        if (s->running && s->adapter && strcmp(s->adapter, adapter->path) != 0) {
            stop_running(d); // finish on the old adapter first
            return;
        }
        if (s->running && s->filtered && !filter) {
            // BlueZ may retain visibility when merely changing the filter.
            stop_running(d); // restart without a discoverable filter
            return;
        }
        if (!s->running) {
            free(s->adapter);
            s->adapter = strdup(adapter->path);
            r = s->adapter ? set_filter(d, filter) : -ENOMEM;
        } else if (filter && !s->filtered) {
            // A discoverable lease joined a search that wasn't.
            r = set_filter(d, true);
        }
    } else if (s->running) {
        stop_running(d);
        return;
    }
    if (r < 0) {
        vela_bt_log("search: %s", strerror(-r));
        if (r != -ENOTCONN) {
            discovery_lost(d);
        }
    }
}

void vela_bt_search_adapter_changed(struct vela_bt_daemon *d, bool was_discovering)
{
    struct vela_bt_search *s = &d->search;
    const struct vela_bt_adapter *adapter = vela_bt_adapter_in_use(d);
    if (!adapter) {
        return;
    }
    if (!adapter->discoverable) {
        set_still_visible(d, false);
    }
    if (!adapter->powered && s->count > 0) {
        vela_bt_search_end_all(d, "adapter-off");
        return;
    }
    bool ours = s->running && s->adapter && strcmp(s->adapter, adapter->path) == 0;
    if (ours && adapter->discovering) {
        s->saw_discovering = true;
    } else if (ours && was_discovering && !adapter->discovering && s->saw_discovering && !s->call) {
        // BlueZ stopped discovering while Vela searched (an adapter reset).
        discovery_lost(d);
    }
}

// ------------------------------------------------------------- leases --

static int on_visible_check(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_bt_daemon *d = data;
    d->search.visible_check = sd_event_source_disable_unref(d->search.visible_check);
    const struct vela_bt_adapter *adapter = vela_bt_adapter_in_use(d);
    if (d->search.count == 0 && adapter && adapter->discoverable) {
        set_still_visible(d, true);
    }
    return 0;
}

static void end_lease(struct vela_bt_daemon *d, size_t index, const char *reason)
{
    struct vela_bt_search *s = &d->search;
    struct vela_bt_lease *lease = s->leases[index];
    memmove(&s->leases[index], &s->leases[index + 1], (s->count - index - 1) * sizeof(*s->leases));
    --s->count;
    vela_bt_log("search %llu ended: %s", (unsigned long long)lease->id, reason);
    sd_bus_emit_signal(d->session, VELA_BT_PATH, VELA_BT_NAME, "SearchEnded", "ts", lease->id, reason);
    lease_free(lease);
    if (s->count == 0) {
        // Rule 3: visibility that outlives Vela's search.
        if (!s->discoverable_before && !s->visible_check
            && sd_event_add_time_relative(d->event, &s->visible_check, CLOCK_MONOTONIC, VISIBLE_CHECK_US, 0,
                   on_visible_check, d)
                < 0) {
            vela_bt_log("visibility check timer: failed");
        }
        s->restart_timer = sd_event_source_disable_unref(s->restart_timer);
    }
    vela_bt_properties_changed(d);
    vela_bt_search_update(d);
}

static size_t lease_index(struct vela_bt_daemon *d, const struct vela_bt_lease *lease)
{
    for (size_t i = 0; i < d->search.count; ++i) {
        if (d->search.leases[i] == lease) {
            return i;
        }
    }
    return d->search.count;
}

static int on_owner_gone(sd_bus_track *track, void *data)
{
    struct vela_bt_lease *lease = data;
    size_t index = lease_index(lease->daemon, lease);
    if (index < lease->daemon->search.count) {
        end_lease(lease->daemon, index, "owner-gone");
    }
    return 0;
}

static int on_time_limit(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_bt_lease *lease = data;
    size_t index = lease_index(lease->daemon, lease);
    if (index < lease->daemon->search.count) {
        end_lease(lease->daemon, index, "time-limit");
    }
    return 0;
}

void vela_bt_search_end_all(struct vela_bt_daemon *d, const char *reason)
{
    while (d->search.count > 0) {
        end_lease(d, d->search.count - 1, reason);
    }
}

int vela_bt_search_start(struct vela_bt_daemon *d, sd_bus_message *call, bool discoverable, sd_bus_error *error)
{
    struct vela_bt_search *s = &d->search;
    const struct vela_bt_adapter *adapter = vela_bt_adapter_in_use(d);
    if (!adapter) {
        return sd_bus_error_set(error, VELA_BT_ERROR("UnknownAdapter"), "There's no Bluetooth adapter.");
    }
    if (!adapter->powered) {
        return sd_bus_error_set(error, VELA_BT_ERROR("Failed"), "Bluetooth is off.");
    }
    struct vela_bt_lease **leases = realloc(s->leases, (s->count + 1) * sizeof(*s->leases));
    if (!leases) {
        return -ENOMEM;
    }
    s->leases = leases;
    struct vela_bt_lease *lease = calloc(1, sizeof(*lease));
    if (!lease) {
        return -ENOMEM;
    }
    lease->daemon = d;
    lease->id = ++s->last_id;
    lease->discoverable = discoverable;
    int r = sd_bus_track_new(d->session, &lease->owner, on_owner_gone, lease);
    if (r >= 0) {
        r = sd_bus_track_add_sender(lease->owner, call);
    }
    if (r >= 0 && s->limit_us > 0) {
        r = sd_event_add_time_relative(d->event, &lease->timer, CLOCK_MONOTONIC, s->limit_us, 0, on_time_limit, lease);
    }
    if (r < 0) {
        lease_free(lease);
        return r;
    }
    if (s->count == 0) {
        // Vela's search begins. What the adapter does now is someone
        // else's, unless Vela's last discovery is still stopping.
        if ((!s->running && !s->call)
            || (s->adapter && strcmp(s->adapter, adapter->path) != 0)) {
            s->others = adapter->discovering;
            s->discoverable_before = adapter->discoverable;
        }
        s->visible_check = sd_event_source_disable_unref(s->visible_check);
        set_still_visible(d, false);
    }
    s->leases[s->count++] = lease;
    vela_bt_log("search %llu started%s", (unsigned long long)lease->id, discoverable ? ", discoverable" : "");
    vela_bt_properties_changed(d);
    vela_bt_search_update(d);
    return sd_bus_reply_method_return(call, "t", lease->id);
}

int vela_bt_search_stop(struct vela_bt_daemon *d, sd_bus_message *call, uint64_t id)
{
    const char *sender = sd_bus_message_get_sender(call);
    for (size_t i = 0; i < d->search.count; ++i) {
        struct vela_bt_lease *lease = d->search.leases[i];
        const char *owner = sd_bus_track_first(lease->owner);
        if (lease->id == id && sender && owner && strcmp(sender, owner) == 0) {
            end_lease(d, i, "stopped");
            break;
        }
    }
    return sd_bus_reply_method_return(call, NULL);
}

// ----------------------------------------------------- being visible --

static int on_visible_set(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    struct vela_bt_search *s = &d->search;
    s->visible_call = sd_bus_slot_unref(s->visible_call);
    bool failed = sd_bus_message_is_method_error(reply, NULL);
    if (failed) {
        vela_bt_log("Set Discoverable: %s", sd_bus_message_get_error(reply)->message);
    } else {
        set_still_visible(d, false);
    }
    vela_bt_waiters_reply(&s->visible_waiters, failed ? &vela_bt_failed : NULL, false);
    return 0;
}

int vela_bt_stop_being_visible(struct vela_bt_daemon *d, sd_bus_message *call, sd_bus_error *error)
{
    struct vela_bt_search *s = &d->search;
    const struct vela_bt_adapter *adapter = vela_bt_adapter_in_use(d);
    if (!adapter) {
        return sd_bus_error_set(error, VELA_BT_ERROR("UnknownAdapter"), "There's no Bluetooth adapter.");
    }
    if (!s->visible_call) {
        vela_bt_log("stop being visible: the user's choice");
        int r = vela_bt_call(d, &s->visible_call, adapter->path, "org.freedesktop.DBus.Properties", "Set",
            VELA_BT_STEP_TIMEOUT_US, on_visible_set, d, "ssv", ADAPTER1, "Discoverable", "b", 0);
        if (r < 0) {
            return sd_bus_error_set(error, VELA_BT_ERROR("Failed"), vela_bt_failed.message);
        }
    }
    vela_bt_waiters_add(&s->visible_waiters, call);
    return 1;
}

// BlueZ ends a client's discovery when it leaves the bus, and Vela's when
// BlueZ goes: nothing to stop, nothing left to check.
void vela_bt_search_adapter_switching(struct vela_bt_daemon *d)
{
    // This visibility check belongs to the previously selected adapter.
    d->search.visible_check = sd_event_source_disable_unref(d->search.visible_check);
    set_still_visible(d, false);
}

void vela_bt_search_forget(struct vela_bt_daemon *d)
{
    struct vela_bt_search *s = &d->search;
    s->call = sd_bus_slot_unref(s->call);
    s->stop_retry_timer = sd_event_source_disable_unref(s->stop_retry_timer);
    s->running = false;
    s->saw_discovering = false;
    free(s->adapter);
    s->adapter = NULL;
    s->discoverable_before = true; // no "still visible" for a BlueZ that went
    vela_bt_search_end_all(d, "adapter-off");
    s->visible_check = sd_event_source_disable_unref(s->visible_check);
    set_still_visible(d, false);
}

void vela_bt_search_free(struct vela_bt_daemon *d)
{
    struct vela_bt_search *s = &d->search;
    for (size_t i = 0; i < s->count; ++i) {
        lease_free(s->leases[i]);
    }
    free(s->leases);
    s->leases = NULL;
    s->count = 0;
    sd_bus_slot_unref(s->call);
    sd_bus_slot_unref(s->visible_call);
    vela_bt_waiters_reply(&s->visible_waiters, &vela_bt_not_running, false);
    sd_event_source_disable_unref(s->visible_check);
    sd_event_source_disable_unref(s->restart_timer);
    sd_event_source_disable_unref(s->stop_retry_timer);
    free(s->adapter);
}
