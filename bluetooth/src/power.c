// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Turning Bluetooth on and off (docs/bluetooth.md §6.3).
//
// BlueZ doesn't power an adapter blocked by rfkill. Turning on first
// removes the soft block of the adapter's own switch, then sets Powered; if
// BlueZ hasn't noticed the unblock yet, Powered is set again, three times
// at most, 400 ms apart. With no adapter at all, the platform switches of
// type Bluetooth are unblocked instead: the built-in radio may be gone
// because of them. Turning off only sets Powered: Vela never blocks.

#include "daemon.h"

#include "log.h"
#include "rfkill.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/rfkill.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define RETRY_US (400 * 1000ULL)
#define RETRIES 3
// After a platform switch is unblocked, the adapter that appears within
// this time is powered.
#define PLATFORM_WAIT_US (10 * 1000000ULL)
#define PLATFORM_SWITCHES 8

void vela_bt_power_end(struct vela_bt_daemon *d, const struct vela_bt_failure *failure, bool cancelled)
{
    struct vela_bt_power *power = &d->power;
    if (!power->active) {
        return;
    }
    power->active = false;
    power->call = sd_bus_slot_unref(power->call);
    power->timer = sd_event_source_disable_unref(power->timer);
    free(power->adapter);
    power->adapter = NULL;
    if (failure && !cancelled) {
        vela_bt_log("power %s: %s", power->on ? "on" : "off", failure->code);
    }
    vela_bt_waiters_reply(&power->waiters, failure, cancelled);
}

static void send_powered(struct vela_bt_daemon *d);

static int on_retry(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_bt_daemon *d = data;
    struct vela_bt_power *power = &d->power;
    power->timer = sd_event_source_disable_unref(power->timer);
    struct vela_bt_adapter *adapter = vela_bt_adapter_find(d, power->adapter);
    if (adapter && adapter->powered == power->on) {
        vela_bt_power_end(d, NULL, false);
    } else if (power->attempt < RETRIES) {
        ++power->attempt;
        send_powered(d);
    } else {
        vela_bt_power_end(d, power->on ? &vela_bt_power_on_failed : &vela_bt_power_off_failed, false);
    }
    return 0;
}

static void retry_later(struct vela_bt_daemon *d)
{
    struct vela_bt_power *power = &d->power;
    power->timer = sd_event_source_disable_unref(power->timer);
    if (sd_event_add_time_relative(d->event, &power->timer, CLOCK_MONOTONIC, RETRY_US, 0, on_retry, d) < 0) {
        vela_bt_power_end(d, &vela_bt_failed, false);
    }
}

static int on_powered_set(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    struct vela_bt_power *power = &d->power;
    power->call = sd_bus_slot_unref(power->call);
    if (sd_bus_message_is_method_error(reply, NULL)) {
        const sd_bus_error *cause = sd_bus_message_get_error(reply);
        vela_bt_log("Set Powered %s: %s: %s", power->on ? "on" : "off", cause->name, cause->message ? cause->message : "");
        // BlueZ refuses until it notices the unblock.
        if (power->on && power->attempt < RETRIES) {
            retry_later(d);
            return 0;
        }
        const struct vela_bt_failure *failure = vela_bt_failure_of(VELA_BT_STEP_POWER, cause->name, cause->message);
        if (!failure || (!power->on && failure == &vela_bt_power_on_failed)) {
            failure = power->on ? &vela_bt_power_on_failed : &vela_bt_power_off_failed;
        }
        vela_bt_power_end(d, failure, false);
        return 0;
    }
    struct vela_bt_adapter *adapter = vela_bt_adapter_find(d, power->adapter);
    if (adapter && adapter->powered == power->on) {
        vela_bt_power_end(d, NULL, false);
    } else {
        retry_later(d);
    }
    return 0;
}

static void send_powered(struct vela_bt_daemon *d)
{
    struct vela_bt_power *power = &d->power;
    int r = vela_bt_call(d, &power->call, power->adapter, "org.freedesktop.DBus.Properties", "Set",
        VELA_BT_STEP_TIMEOUT_US, on_powered_set, d, "ssv", "org.bluez.Adapter1", "Powered", "b", (int)power->on);
    if (r < 0) {
        vela_bt_power_end(d, r == -ENOTCONN ? &vela_bt_not_running : &vela_bt_failed, false);
    }
}

// With no adapter: unblocks the soft-blocked platform switches of type
// Bluetooth. NULL, or the failure.
static const struct vela_bt_failure *unblock_platform(struct vela_bt_daemon *d)
{
    struct vela_bt_switch switches[PLATFORM_SWITCHES];
    int count = vela_bt_platform_switches(d->sysfs, switches, PLATFORM_SWITCHES);
    bool unblocked = false;
    bool hard = false;
    for (int i = 0; i < count; ++i) {
        if (switches[i].hard) {
            hard = true;
        } else if (switches[i].soft) {
            int r = vela_bt_unblock(d->rfkill, switches[i].index);
            if (r < 0) {
                vela_bt_log("unblocking platform switch %d: %s", switches[i].index, strerror(-r));
            } else {
                vela_bt_log("unblocked platform switch %d", switches[i].index);
                unblocked = true;
            }
        }
    }
    if (unblocked) {
        d->power_on_until = vela_bt_now() + PLATFORM_WAIT_US;
        return NULL;
    }
    return hard ? &vela_bt_hard_blocked : d->bluez ? &vela_bt_no_adapter : &vela_bt_not_running;
}

int vela_bt_power_set(struct vela_bt_daemon *d, sd_bus_message *call, bool on, sd_bus_error *error)
{
    struct vela_bt_power *power = &d->power;
    if (power->active && power->on == on) {
        vela_bt_waiters_add(&power->waiters, call);
        return 1;
    }
    // The user changed their mind: the earlier request stops here.
    vela_bt_power_end(d, NULL, true);

    struct vela_bt_adapter *adapter = vela_bt_adapter_in_use(d);
    if (!adapter) {
        const struct vela_bt_failure *failure = on ? unblock_platform(d) : NULL;
        vela_bt_platform_refresh(d);
        if (failure) {
            return sd_bus_error_set(error, VELA_BT_ERROR("Failed"), failure->message);
        }
        return call ? sd_bus_reply_method_return(call, NULL) : 0;
    }
    if (adapter->powered == on) {
        return call ? sd_bus_reply_method_return(call, NULL) : 0;
    }
    if (on) {
        struct vela_bt_switch own;
        const char *hci = vela_bt_hci_name(adapter->path);
        if (hci && vela_bt_adapter_switch(d->sysfs, hci, &own) == 0) {
            if (own.hard) {
                return sd_bus_error_set(error, VELA_BT_ERROR("Failed"), vela_bt_hard_blocked.message);
            }
            if (own.soft) {
                int r = vela_bt_unblock(d->rfkill, own.index);
                vela_bt_log("unblocking %s (switch %d): %s", hci, own.index, r < 0 ? strerror(-r) : "done");
            }
        }
    }
    *power = (struct vela_bt_power) { .active = true, .on = on, .adapter = strdup(adapter->path) };
    if (!power->adapter) {
        power->active = false;
        return -ENOMEM;
    }
    vela_bt_waiters_add(&power->waiters, call);
    send_powered(d);
    return 1;
}

void vela_bt_power_adapter_changed(struct vela_bt_daemon *d)
{
    struct vela_bt_power *power = &d->power;
    if (power->active) {
        struct vela_bt_adapter *adapter = vela_bt_adapter_find(d, power->adapter);
        if (!adapter) {
            vela_bt_power_end(d, &vela_bt_adapter_gone, false);
        } else if (adapter->powered == power->on) {
            vela_bt_power_end(d, NULL, false);
        }
    }
    // The adapter that a platform switch was hiding.
    struct vela_bt_adapter *adapter = vela_bt_adapter_in_use(d);
    if (adapter && d->power_on_until) {
        bool wanted = vela_bt_now() < d->power_on_until;
        d->power_on_until = 0;
        if (wanted && !adapter->powered && !power->active) {
            sd_bus_error error = SD_BUS_ERROR_NULL;
            vela_bt_power_set(d, NULL, true, &error);
            sd_bus_error_free(&error);
        }
    }
}

void vela_bt_platform_refresh(struct vela_bt_daemon *d)
{
    bool blocked = false;
    if (!vela_bt_available(d)) {
        struct vela_bt_switch switches[PLATFORM_SWITCHES];
        int count = vela_bt_platform_switches(d->sysfs, switches, PLATFORM_SWITCHES);
        for (int i = 0; i < count; ++i) {
            blocked |= switches[i].soft || switches[i].hard;
        }
    }
    if (blocked != d->platform_blocked) {
        d->platform_blocked = blocked;
        vela_bt_properties_changed(d);
    }
}

static int on_rfkill_event(sd_event_source *source, int fd, uint32_t revents, void *data)
{
    struct vela_bt_daemon *d = data;
    struct rfkill_event event;
    while (read(fd, &event, sizeof(event)) > 0) {
    }
    vela_bt_platform_refresh(d);
    return 0;
}

int vela_bt_rfkill_watch(struct vela_bt_daemon *d)
{
    // The tests' fake /dev/rfkill is a plain file: nothing to watch there.
    int fd = open(d->rfkill, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    int r = sd_event_add_io(d->event, &d->rfkill_watch, fd, EPOLLIN, on_rfkill_event, d);
    if (r < 0) {
        close(fd);
        return r;
    }
    sd_event_source_set_io_fd_own(d->rfkill_watch, 1);
    return 0;
}
