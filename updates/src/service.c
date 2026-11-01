// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// org.vela.Updates on the session bus (docs/updates.md §7). Everything is
// a property, sent again whole on any change: they are few and small.
// Methods need a caller that is not sandboxed (§7, "Who may call").

#include "daemon.h"

#include "caller/caller.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

const char *vela_up_activity(const struct vela_up_daemon *d)
{
    if (d->run.present) {
        return strcmp(d->run.action, "install") == 0 ? "installing" : "checking";
    }
    if (d->check_pid > 0) {
        return "checking";
    }
    return d->starting ? "starting" : "idle";
}

const char *vela_up_announce(const struct vela_up_daemon *d)
{
    if (!d->check.valid || strcmp(d->check.status, "update-available") != 0 || !*d->check.commit) {
        return "";
    }
    return d->announced && strcmp(d->announced, d->check.commit) == 0 ? "" : d->check.commit;
}

static const char *or_empty(const char *s)
{
    return s ? s : "";
}

static int get_string(sd_bus *bus, const char *path, const char *interface, const char *property,
    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    const struct vela_up_check *c = &d->check;
    const char *value = "";
    if (strcmp(property, "Activity") == 0) {
        value = vela_up_activity(d);
    } else if (strcmp(property, "Phase") == 0) {
        value = d->run.present && strcmp(d->run.action, "install") == 0 ? d->run.phase : "";
    } else if (strcmp(property, "Status") == 0) {
        value = c->valid ? c->status : "unknown";
    } else if (strcmp(property, "Channel") == 0) {
        value = c->valid ? c->channel : "";
    } else if (strcmp(property, "InstalledPackage") == 0) {
        value = c->valid ? c->installed_package : "";
    } else if (strcmp(property, "InstalledVersion") == 0) {
        value = c->valid ? c->installed_version : "";
    } else if (strcmp(property, "Version") == 0) {
        value = c->valid ? c->version : "";
    } else if (strcmp(property, "Tag") == 0) {
        value = c->valid ? c->tag : "";
    } else if (strcmp(property, "Commit") == 0) {
        value = c->valid ? c->commit : "";
    } else if (strcmp(property, "Source") == 0) {
        value = d->source;
    } else if (strcmp(property, "InstallResult") == 0) {
        value = d->install.present ? d->install.result : "";
    } else if (strcmp(property, "Announce") == 0) {
        value = vela_up_announce(d);
    }
    return sd_bus_message_append(reply, "s", or_empty(value));
}

static int get_pairs(sd_bus *bus, const char *path, const char *interface, const char *property,
    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    bool changes = strcmp(property, "Changes") == 0;
    const struct vela_up_pair *pairs = changes ? d->check.changes : d->check.notes;
    size_t count = d->check.valid ? (changes ? d->check.change_count : d->check.note_count) : 0;
    int r = sd_bus_message_open_container(reply, 'a', "(ss)");
    for (size_t i = 0; r >= 0 && i < count; ++i) {
        r = sd_bus_message_append(reply, "(ss)", pairs[i].first, pairs[i].second);
    }
    return r < 0 ? r : sd_bus_message_close_container(reply);
}

static int get_error(sd_bus *bus, const char *path, const char *interface, const char *property,
    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    if (strcmp(property, "Error") == 0) {
        return sd_bus_message_append(reply, "(ss)", or_empty(d->error_code), or_empty(d->error_message));
    }
    bool failed = d->install.present && strcmp(d->install.result, "success") != 0;
    return sd_bus_message_append(reply, "(ss)", failed ? d->install.error_code : "",
        failed ? d->install.error_message : "");
}

static int get_number(sd_bus *bus, const char *path, const char *interface, const char *property,
    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    if (strcmp(property, "ChangesTotal") == 0) {
        return sd_bus_message_append(reply, "u", d->check.valid ? d->check.changes_total : 0);
    }
    if (strcmp(property, "CheckInterval") == 0) {
        return sd_bus_message_append(reply, "u", d->check_interval_hours);
    }
    if (strcmp(property, "NextCheck") == 0) {
        return sd_bus_message_append(reply, "x", d->next_check_at);
    }
    if (strcmp(property, "InstallFinished") == 0) {
        return sd_bus_message_append(reply, "x", d->install.present ? d->install.finished_at : (int64_t)0);
    }
    return sd_bus_message_append(reply, "x", d->check.valid ? d->check.checked_at : (int64_t)0);
}

static int get_flag(sd_bus *bus, const char *path, const char *interface, const char *property,
    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    bool value = strcmp(property, "NeedsRelogin") == 0 ? d->needs_relogin
        : strcmp(property, "WaitingForNetwork") == 0 ? d->waiting_for_network : d->auto_check;
    return sd_bus_message_append(reply, "b", (int)value);
}

static int check_caller(sd_bus_message *call, sd_bus_error *error)
{
    return vela_caller_check(call, VELA_CALLER_UNSANDBOXED, NULL, VELA_UP_ERROR("NotAllowed"), error);
}

static int method_check(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    int r = check_caller(call, error);
    if (r < 0) {
        return r;
    }
    r = vela_up_check_start(d);
    if (r == -EBUSY) {
        return sd_bus_error_set(error, VELA_UP_ERROR("Busy"), "An update or a check is already running.");
    }
    if (r < 0) {
        return sd_bus_error_setf(error, VELA_UP_ERROR("Failed"), "vela-update could not be started: %s",
            strerror(-r));
    }
    return sd_bus_reply_method_return(call, NULL);
}

static int method_install(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    int r = check_caller(call, error);
    if (r < 0) {
        return r;
    }
    struct vela_up_options options = { 0 };
    if ((r = sd_bus_message_enter_container(call, 'a', "{sv}")) < 0) {
        return r;
    }
    while ((r = sd_bus_message_enter_container(call, 'e', "sv")) > 0) {
        const char *key = NULL;
        const char *value = NULL;
        if ((r = sd_bus_message_read(call, "s", &key)) < 0) {
            return r;
        }
        const char **slot = strcmp(key, "channel") == 0 ? &options.channel
            : strcmp(key, "branch") == 0                 ? &options.branch
            : strcmp(key, "commit") == 0                 ? &options.commit
            : strcmp(key, "source") == 0                 ? &options.source
            : strcmp(key, "tag") == 0                    ? &options.tag
                                                         : NULL;
        if (!slot) {
            return sd_bus_error_setf(error, VELA_UP_ERROR("InvalidArgs"), "unknown option: %s", key);
        }
        if (sd_bus_message_read(call, "v", "s", &value) < 0) {
            return sd_bus_error_setf(error, VELA_UP_ERROR("InvalidArgs"), "%s must be a string", key);
        }
        // An empty value is no value.
        *slot = value && *value ? value : NULL;
        if ((r = sd_bus_message_exit_container(call)) < 0) {
            return r;
        }
    }
    if (r < 0) {
        return r;
    }
    if ((r = sd_bus_message_exit_container(call)) < 0) {
        return r;
    }
    r = vela_up_install_start(d, &options, error);
    return r < 0 ? r : sd_bus_reply_method_return(call, NULL);
}

static int method_set_auto_check(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    int on;
    int r = sd_bus_message_read(call, "b", &on);
    if (r >= 0) {
        r = check_caller(call, error);
    }
    if (r < 0) {
        return r;
    }
    vela_up_set_auto_check(d, on);
    return sd_bus_reply_method_return(call, NULL);
}

static int method_set_check_interval(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    uint32_t hours;
    int r = sd_bus_message_read(call, "u", &hours);
    if (r >= 0) {
        r = check_caller(call, error);
    }
    if (r < 0) {
        return r;
    }
    if (!vela_up_valid_check_interval(hours)) {
        return sd_bus_error_set(error, VELA_UP_ERROR("InvalidArgs"), "Unsupported check interval.");
    }
    if (vela_up_set_check_interval(d, hours) < 0) {
        return sd_bus_error_set(error, VELA_UP_ERROR("Failed"), "Could not save the check interval.");
    }
    return sd_bus_reply_method_return(call, NULL);
}

static int method_announced(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    const char *commit;
    int r = sd_bus_message_read(call, "s", &commit);
    if (r >= 0) {
        r = check_caller(call, error);
    }
    if (r < 0) {
        return r;
    }
    vela_up_announced(d, commit);
    return sd_bus_reply_method_return(call, NULL);
}

#define CHANGES SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Activity", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("Phase", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("Status", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("Channel", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("InstalledPackage", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("InstalledVersion", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("Version", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("Tag", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("Commit", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("Changes", "a(ss)", get_pairs, 0, CHANGES),
    SD_BUS_PROPERTY("ChangesTotal", "u", get_number, 0, CHANGES),
    SD_BUS_PROPERTY("Notes", "a(ss)", get_pairs, 0, CHANGES),
    SD_BUS_PROPERTY("Source", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("LastCheck", "x", get_number, 0, CHANGES),
    SD_BUS_PROPERTY("CheckInterval", "u", get_number, 0, CHANGES),
    SD_BUS_PROPERTY("NextCheck", "x", get_number, 0, CHANGES),
    SD_BUS_PROPERTY("WaitingForNetwork", "b", get_flag, 0, CHANGES),
    SD_BUS_PROPERTY("Error", "(ss)", get_error, 0, CHANGES),
    SD_BUS_PROPERTY("InstallResult", "s", get_string, 0, CHANGES),
    SD_BUS_PROPERTY("InstallError", "(ss)", get_error, 0, CHANGES),
    SD_BUS_PROPERTY("InstallFinished", "x", get_number, 0, CHANGES),
    SD_BUS_PROPERTY("NeedsRelogin", "b", get_flag, 0, CHANGES),
    SD_BUS_PROPERTY("AutoCheck", "b", get_flag, 0, CHANGES),
    SD_BUS_PROPERTY("Announce", "s", get_string, 0, CHANGES),
    SD_BUS_METHOD("Check", "", "", method_check, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Install", "a{sv}", "", method_install, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetAutoCheck", "b", "", method_set_auto_check, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetCheckInterval", "u", "", method_set_check_interval, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Announced", "s", "", method_announced, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

static int on_flush(sd_event_source *source, void *data)
{
    struct vela_up_daemon *d = data;
    d->flush = sd_event_source_disable_unref(d->flush);
    // NULL: every property that emits changes.
    int r = sd_bus_emit_properties_changed_strv(d->session, VELA_UP_PATH, VELA_UP_NAME, NULL);
    if (r < 0) {
        vela_up_log("PropertiesChanged: %s", strerror(-r));
    }
    return 0;
}

void vela_up_changed(struct vela_up_daemon *d)
{
    if (!d->flush && d->event) {
        sd_event_add_defer(d->event, &d->flush, on_flush, d);
    }
}

int vela_up_service_start(struct vela_up_daemon *d)
{
    int r = sd_bus_add_object_vtable(d->session, NULL, VELA_UP_PATH, VELA_UP_NAME, vtable, d);
    if (r < 0) {
        return r;
    }
    return sd_bus_request_name(d->session, VELA_UP_NAME, 0);
}
