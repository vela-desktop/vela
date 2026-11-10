// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "session.h"

#include "alloc/alloc.h"
#include "supervisor.h"
#include "util.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

void vela_session_set_environment(void)
{
    // Vela uses KDE's services (keyring, portals, app theme): "KDE" after
    // "Vela" makes Brave, Chrome and Electron apps (VS Code) use KDE's keyring
    // as they do inside Plasma. Without it they encrypt their data with a
    // fixed key and can no longer read what they saved in Plasma: site logins
    // lost, sync and passwords that can't be decrypted. Plasma's autostart
    // entries don't run: they have X-systemd-skip, except session restore,
    // which vela-session-env turns off. Plasma Login copies DesktopNames from
    // the .desktop file as it is ("Vela;KDE;"), while apps expect the names
    // separated by ':'.
    const char *current = getenv("XDG_CURRENT_DESKTOP");
    size_t size = vela_size_add(current ? strlen(current) : 0, 16);
    char *desktops = VELA_NEW_N(char, size);
    vela_desktop_names(current, desktops, size);
    if (!desktops[0] || strcmp(desktops, "Vela") == 0) {
        snprintf(desktops, size, "Vela:KDE");
    }
    setenv("XDG_CURRENT_DESKTOP", desktops, 1);
    free(desktops);
    setenv("KDE_SESSION_VERSION", "6", 0);
    setenv("XDG_SESSION_DESKTOP", "vela", 0);
    setenv("XDG_SESSION_TYPE", "wayland", 1); // from a console it's "tty"
    // Qt and KDE apps with the theme chosen in KDE (style, colors, fonts,
    // icons), as inside Plasma.
    if (access("/usr/lib/qt6/plugins/platformthemes/KDEPlasmaPlatformTheme6.so", F_OK) == 0) {
        setenv("QT_QPA_PLATFORMTHEME", "kde", 0);
    }
    // KDE's application menu: without it, Dolphin and the others don't know
    // what to open files with.
    if (access("/etc/xdg/menus/plasma-applications.menu", F_OK) == 0) {
        setenv("XDG_MENU_PREFIX", "plasma-", 0);
    }
}

void vela_session_run_hook(const char *action)
{
    char hook[PATH_MAX];
    if (!vela_session_hook_path(hook, sizeof(hook))) {
        wlr_log(WLR_INFO, "Session: vela-session-env not found, no systemd integration");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execl(hook, hook, action, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        wlr_log(WLR_INFO, "Session: %s %s (%s)", hook, action,
            WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "done" : "failed");
    }
}
