// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// What vela-update reports in its state directory (docs/updates.md §5.3),
// followed with inotify, and a running update's process with a pidfd: a
// run killed without its exit trap leaves run.json behind, and no file
// event says it's over.

#include "daemon.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static char *path_in(const char *dir, const char *name)
{
    char *path = NULL;
    return asprintf(&path, "%s/%s", dir, name) < 0 ? NULL : path;
}

static void set_string(char **field, const char *value)
{
    free(*field);
    *field = value && *value ? strdup(value) : NULL;
}

// A check's outcome, from check.json or our own check's output, in the
// order they come: a failure is kept beside the last completed check,
// which stays shown (§6.1).
void vela_up_adopt_check(struct vela_up_daemon *d, struct vela_up_check *check)
{
    if (!check->valid) {
        return;
    }
    if (strcmp(check->status, "error") == 0) {
        set_string(&d->error_code, check->error_code);
        set_string(&d->error_message, check->error_message);
        d->error_at = check->checked_at;
        vela_up_check_clear(check);
        return;
    }
    set_string(&d->error_code, NULL);
    set_string(&d->error_message, NULL);
    vela_up_check_clear(&d->check);
    d->check = *check;
    memset(check, 0, sizeof(*check));
}

static int on_run_exit(sd_event_source *source, int fd, uint32_t revents, void *data)
{
    vela_up_state_reload(data);
    return 0;
}

static void watch_run(struct vela_up_daemon *d)
{
    d->run_watch = sd_event_source_disable_unref(d->run_watch);
    if (!d->run.present) {
        return;
    }
    int fd = (int)syscall(SYS_pidfd_open, d->run.pid, 0);
    if (fd < 0) {
        return;
    }
    // Still the run's process now that the pidfd holds it.
    if (!vela_up_run_alive("/proc", &d->run)) {
        close(fd);
        return;
    }
    if (sd_event_add_io(d->event, &d->run_watch, fd, EPOLLIN, on_run_exit, d) < 0) {
        close(fd);
        return;
    }
    sd_event_source_set_io_fd_own(d->run_watch, true);
}

static void read_source(struct vela_up_daemon *d)
{
    char *path = path_in(d->config_dir, "updates.conf");
    FILE *f = path ? fopen(path, "re") : NULL;
    free(path);
    char *source = NULL;
    char line[4200];
    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "source=", 7) == 0) {
            line[strcspn(line, "\n")] = '\0';
            source = strdup(line + 7);
            break;
        }
    }
    if (f) {
        fclose(f);
    }
    free(d->source);
    d->source = source && *source ? source : strdup("https://github.com/vela-desktop/vela.git");
    if (source && !*source) {
        free(source);
    }
}

void vela_up_state_reload(struct vela_up_daemon *d)
{
    char *path = path_in(d->state_dir, "check.json");
    // Each check.json is created as a new temporary file and atomically
    // renamed into place. Track its identity, not checked_at: successive
    // checks within one second must both be observed. Reloads triggered by
    // run.json or install.json must not reapply an old check.
    struct stat observed;
    if (path && stat(path, &observed) == 0
        && (observed.st_dev != d->file_seen_dev || observed.st_ino != d->file_seen_ino
            || observed.st_mtim.tv_sec != d->file_seen_mtime.tv_sec
            || observed.st_mtim.tv_nsec != d->file_seen_mtime.tv_nsec)) {
        struct vela_up_check check;
        if (vela_up_check_read(path, &check) >= 0) {
            d->file_seen_dev = observed.st_dev;
            d->file_seen_ino = observed.st_ino;
            d->file_seen_mtime = observed.st_mtim;
            vela_up_adopt_check(d, &check);
            vela_up_check_clear(&check);
        }
    }
    free(path);

    path = path_in(d->state_dir, "install.json");
    struct vela_up_install install = { 0 };
    bool changed = path && stat(path, &observed) == 0
        && (observed.st_dev != d->install_seen_dev || observed.st_ino != d->install_seen_ino
            || observed.st_mtim.tv_sec != d->install_seen_mtime.tv_sec
            || observed.st_mtim.tv_nsec != d->install_seen_mtime.tv_nsec);
    if (path && vela_up_install_read(path, &install) >= 0
        && (!d->install.present || install.finished_at >= d->install.finished_at)) {
        if (changed) {
            d->install_seen_dev = observed.st_dev;
            d->install_seen_ino = observed.st_ino;
            d->install_seen_mtime = observed.st_mtim;
            // A new report while we run: an install just ended.
            if (d->loaded && strcmp(install.result, "success") == 0) {
                d->recheck = true;
            }
        }
        vela_up_install_clear(&d->install);
        d->install = install;
    } else if (install.present) {
        vela_up_install_clear(&install);
    }
    free(path);

    path = path_in(d->state_dir, "run.json");
    struct vela_up_run run;
    int r = path ? vela_up_run_read(path, &run) : -ENOMEM;
    free(path);
    vela_up_run_clear(&d->run);
    if (r >= 0 && run.present) {
        if (vela_up_run_alive("/proc", &run)) {
            d->run = run;
        } else {
            // Killed without its exit trap: an install that never wrote its
            // end was cancelled.
            if (strcmp(run.action, "install") == 0
                && (!d->install.present || d->install.finished_at < run.started_at)) {
                vela_up_install_clear(&d->install);
                d->install.present = true;
                d->install.channel = strdup(run.channel);
                d->install.result = strdup("cancelled");
                d->install.error_code = strdup("cancelled");
                d->install.error_message = strdup("The update was interrupted.");
                d->install.installed_version = strdup("");
                d->install.finished_at = (int64_t)time(NULL);
            }
            vela_up_run_clear(&run);
        }
    } else if (r >= 0) {
        vela_up_run_clear(&run);
    }
    watch_run(d);

    if (d->starting
        && ((d->run.present && strcmp(d->run.action, "install") == 0)
            || (d->install.present && d->install.finished_at >= d->starting_at))) {
        vela_up_install_started(d);
    }
    read_source(d);
    vela_up_relogin_refresh(d);
    d->loaded = true;
    // install.json comes before run.json goes: wait for the run's end.
    if (d->recheck && !d->run.present && vela_up_check_start(d) >= 0) {
        d->recheck = false;
    }
    vela_up_changed(d);
}

static int on_inotify(sd_event_source *source, int fd, uint32_t revents, void *data)
{
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (read(fd, buffer, sizeof(buffer)) > 0) {
    }
    vela_up_state_reload(data);
    return 0;
}

static int make_dirs(const char *path)
{
    char *copy = strdup(path);
    if (!copy) {
        return -ENOMEM;
    }
    for (char *slash = strchr(copy + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        mkdir(copy, 0700);
        *slash = '/';
    }
    int r = mkdir(copy, 0700) < 0 && errno != EEXIST ? -errno : 0;
    free(copy);
    return r;
}

int vela_up_state_start(struct vela_up_daemon *d)
{
    int r = make_dirs(d->state_dir);
    if (r < 0) {
        return r;
    }
    d->inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (d->inotify < 0) {
        return -errno;
    }
    // Files are replaced with a rename (moved to), run.json is removed.
    if (inotify_add_watch(d->inotify, d->state_dir, IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE) < 0) {
        return -errno;
    }
    r = sd_event_add_io(d->event, &d->inotify_io, d->inotify, EPOLLIN, on_inotify, d);
    if (r < 0) {
        return r;
    }

    char *path = path_in(d->state_dir, "announced");
    FILE *f = path ? fopen(path, "re") : NULL;
    free(path);
    char line[128];
    if (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        set_string(&d->announced, line);
    }
    if (f) {
        fclose(f);
    }
    vela_up_state_reload(d);
    return 0;
}

void vela_up_announced(struct vela_up_daemon *d, const char *commit)
{
    if (!commit || !*commit || (d->announced && strcmp(d->announced, commit) == 0)) {
        return;
    }
    set_string(&d->announced, commit);
    char *path = path_in(d->state_dir, "announced");
    // mkstemp creates a unique file with mode 0600 (regardless of umask).
    // Never open a predictable, potentially pre-existing file for writing.
    char *temp = path_in(d->state_dir, ".announced.XXXXXX");
    int fd = temp ? mkstemp(temp) : -1;
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f && fd >= 0) {
        close(fd);
    }
    if (f) {
        bool written = fprintf(f, "%s\n", commit) > 0;
        written = fclose(f) == 0 && written;
        if (!written || rename(temp, path) < 0) {
            vela_up_log("can't save the announced update: %s", strerror(errno));
            unlink(temp);
        }
    } else {
        vela_up_log("can't save the announced update: %s", strerror(errno));
    }
    free(temp);
    free(path);
    vela_up_changed(d);
}

void vela_up_relogin_refresh(struct vela_up_daemon *d)
{
    bool needs = vela_up_exe_replaced("/proc", d->session_pid);
    if (needs != d->needs_relogin) {
        d->needs_relogin = needs;
        vela_up_changed(d);
    }
}

void vela_up_state_free(struct vela_up_daemon *d)
{
    d->run_watch = sd_event_source_disable_unref(d->run_watch);
    d->inotify_io = sd_event_source_disable_unref(d->inotify_io);
    if (d->inotify >= 0) {
        close(d->inotify);
    }
    vela_up_check_clear(&d->check);
    vela_up_run_clear(&d->run);
    vela_up_install_clear(&d->install);
    free(d->error_code);
    free(d->error_message);
    free(d->announced);
    free(d->source);
}
