// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "caller.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool vela_caller_sandboxed(const char *proc, pid_t pid)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%d/root/.flatpak-info", proc, (int)pid) >= (int)sizeof(path)) {
        return true;
    }
    if (access(path, F_OK) == 0) {
        return true;
    }
    // Only absence is evidence of no Flatpak marker. EACCES, ESRCH, etc.
    // are unknown process identity, never permission to operate Bluetooth.
    if (errno != ENOENT) {
        return true;
    }
    if (snprintf(path, sizeof(path), "%s/%d/cgroup", proc, (int)pid) >= (int)sizeof(path)) {
        return true;
    }
    FILE *cgroups = fopen(path, "re");
    if (!cgroups) {
        // Gone, or not ours to read: not trusted.
        return true;
    }
    bool snap = false;
    char line[4096];
    while (!snap && fgets(line, sizeof(line), cgroups)) {
        // "0::/user.slice/.../app.slice/snap.firefox.firefox-1234.scope"
        for (char *part = strchr(line, '/'); part && !snap; part = strchr(part + 1, '/')) {
            snap = strncmp(part + 1, "snap.", 5) == 0;
        }
    }
    fclose(cgroups);
    return snap;
}

bool vela_caller_exe_allowed(const char *exe, const char *const *allowed)
{
    static const char deleted[] = " (deleted)";
    size_t length = strlen(exe);
    if (length >= strlen(deleted) && strcmp(exe + length - strlen(deleted), deleted) == 0) {
        length -= strlen(deleted);
    }
    for (; allowed && *allowed; ++allowed) {
        if (**allowed == '/' && strlen(*allowed) == length && strncmp(exe, *allowed, length) == 0) {
            return true;
        }
    }
    return false;
}

pid_t vela_caller_pidfd_pid(const char *proc, int pidfd)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/self/fdinfo/%d", proc, pidfd) >= (int)sizeof(path)) {
        return -1;
    }
    FILE *info = fopen(path, "re");
    if (!info) {
        return -1;
    }
    pid_t pid = -1;
    char line[256];
    while (fgets(line, sizeof(line), info)) {
        long value;
        if (sscanf(line, "Pid:\t%ld", &value) == 1) {
            pid = value > 0 && value <= INT_MAX ? (pid_t)value : -1;
            break;
        }
    }
    fclose(info);
    return pid;
}

static int refuse(sd_bus_message *call, const char *why, pid_t pid, const char *error_name, sd_bus_error *error)
{
    char exe[PATH_MAX] = "?";
    char path[64];
    if (pid > 0) {
        snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
        ssize_t length = readlink(path, exe, sizeof(exe) - 1);
        exe[length > 0 ? length : 0] = '\0';
    }
    fprintf(stderr, "%s: refused %s from %s (%s, pid %d): %s\n", program_invocation_short_name,
        sd_bus_message_get_member(call), sd_bus_message_get_sender(call) ? sd_bus_message_get_sender(call) : "?",
        exe[0] ? exe : "?", (int)pid, why);
    return sd_bus_error_set(error, error_name, "Not allowed for this app.");
}

int vela_caller_check(sd_bus_message *call, enum vela_caller_trust trust, const char *const *allowed,
    const char *error_name, sd_bus_error *error)
{
    uint64_t mask = SD_BUS_CREDS_UID | SD_BUS_CREDS_EUID | SD_BUS_CREDS_PID;
#ifdef VELA_HAVE_PIDFD_CREDS
    mask |= SD_BUS_CREDS_PIDFD;
#endif
    sd_bus_creds *creds = NULL;
    if (sd_bus_query_sender_creds(call, mask, &creds) < 0) {
        return refuse(call, "no credentials", -1, error_name, error);
    }
    uid_t uid;
    if (sd_bus_creds_get_euid(creds, &uid) < 0 && sd_bus_creds_get_uid(creds, &uid) < 0) {
        sd_bus_creds_unref(creds);
        return refuse(call, "no user", -1, error_name, error);
    }
    if (uid != geteuid()) {
        sd_bus_creds_unref(creds);
        return refuse(call, "another user", -1, error_name, error);
    }
    if (trust == VELA_CALLER_SAME_USER) {
        sd_bus_creds_unref(creds);
        return 0;
    }

    // The process, through its pidfd when the bus gives one: a PID could be
    // reused by another process while it's being checked.
    pid_t pid = -1;
    int pidfd = -1;
#ifdef VELA_HAVE_PIDFD_CREDS
    if (sd_bus_creds_get_pidfd_dup(creds, &pidfd) >= 0) {
        pid = vela_caller_pidfd_pid("/proc", pidfd);
    }
#endif
    if (pidfd < 0 && sd_bus_creds_get_pid(creds, &pid) < 0) {
        pid = -1;
    }
    sd_bus_creds_unref(creds);
    const char *why = NULL;
    if (pid <= 0) {
        why = "no process";
    } else if (vela_caller_sandboxed("/proc", pid)) {
        why = "sandboxed";
    } else if (trust == VELA_CALLER_VELA_UI) {
        char path[64];
        char exe[PATH_MAX];
        snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
        ssize_t length = readlink(path, exe, sizeof(exe) - 1);
        if (length <= 0) {
            why = "no executable";
        } else {
            exe[length] = '\0';
            if (!vela_caller_exe_allowed(exe, allowed)) {
                why = "not Vela's UI";
            }
        }
    }
    // Still the same process after the checks. On older systemd without
    // pidfd credentials, also verify that the D-Bus sender still exists and
    // has the same kernel-reported PID and user, rather than trusting a bare
    // PID after multiple /proc lookups.
    if (!why && pidfd >= 0 && vela_caller_pidfd_pid("/proc", pidfd) != pid) {
        why = "process gone";
    } else if (!why && pidfd < 0) {
        sd_bus_creds *current = NULL;
        pid_t current_pid = -1;
        uid_t current_uid = (uid_t)-1;
        if (sd_bus_query_sender_creds(call, SD_BUS_CREDS_PID | SD_BUS_CREDS_EUID | SD_BUS_CREDS_UID, &current) < 0
            || sd_bus_creds_get_pid(current, &current_pid) < 0
            || (sd_bus_creds_get_euid(current, &current_uid) < 0
                && sd_bus_creds_get_uid(current, &current_uid) < 0)
            || current_pid != pid || current_uid != geteuid()) {
            why = "sender changed or disappeared";
        }
        sd_bus_creds_unref(current);
    }
    if (pidfd >= 0) {
        close(pidfd);
    }
    return why ? refuse(call, why, pid, error_name, error) : 0;
}
