// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "session.h"

#include "supervisor.h"
#include "util.h"

#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

static long regular_size(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return 0;
    }
    return st.st_size > LONG_MAX ? LONG_MAX : (long)st.st_size;
}

// KWallet files created by a previous Plasma session. A wallet Vela itself
// just created is tiny next to a real GNOME login keyring.
static long kwallet_bytes(void)
{
    const char *home = getenv("HOME");
    char dir[PATH_MAX];
    if (!home || !home[0] || !vela_format(dir, sizeof(dir), "%s/.local/share/kwalletd", home)) {
        return 0;
    }
    DIR *entries = opendir(dir);
    if (!entries) {
        return 0;
    }
    long total = 0;
    for (struct dirent *entry = readdir(entries); entry; entry = readdir(entries)) {
        const size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 4, ".kwl") != 0) {
            continue;
        }
        char path[PATH_MAX];
        if (!vela_format(path, sizeof(path), "%s/%s", dir, entry->d_name)) {
            continue;
        }
        const long size = regular_size(path);
        total = total > LONG_MAX - size ? LONG_MAX : total + size;
    }
    closedir(entries);
    return total;
}

static long gnome_keyring_bytes(void)
{
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    if (!home || !home[0] || !vela_format(path, sizeof(path), "%s/.local/share/keyrings/login.keyring", home)) {
        return 0;
    }
    return regular_size(path);
}

// Prints KEY=VALUE lines. The compositor keeps them so the shell and apps
// inherit SSH_AUTH_SOCK and the daemon stays the Secret Service.
static void start_gnome_keyring(void)
{
    FILE *pipe = popen("gnome-keyring-daemon --start --components=pkcs11,secrets,ssh", "r");
    if (!pipe) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), pipe)) {
        char *newline = strchr(line, '\n');
        if (newline) {
            *newline = '\0';
        }
        char *equals = strchr(line, '=');
        if (!equals || equals == line) {
            continue;
        }
        *equals = '\0';
        setenv(line, equals + 1, 1);
    }
    pclose(pipe);
}

static void start_kwallet(void)
{
    pid_t pid = fork();
    if (pid != 0) {
        return;
    }
    setsid();
    execlp("kwalletd6", "kwalletd6", (char *)NULL);
    _exit(127);
}

void vela_session_set_environment(void)
{
    // Browsers pick a wallet from the first desktop name they know. "KDE"
    // selects KWallet, which is right for secrets saved in Plasma and wrong
    // when they already live in the GNOME login keyring: Brave and gh then
    // talk to an empty wallet and look logged out. The login screen only
    // publishes "Vela", so KDE autostart cannot claim the Secret Service
    // before this choice. Names are separated by ':' (Plasma Login would
    // copy "Vela;KDE;" through as it is).
    const char *backend = vela_secret_backend(gnome_keyring_bytes(), kwallet_bytes());
    char desktops[32];
    snprintf(desktops, sizeof(desktops), "Vela:%s", backend);
    setenv("XDG_CURRENT_DESKTOP", desktops, 1);
    if (strcmp(backend, "KDE") == 0) {
        setenv("KDE_SESSION_VERSION", "6", 0);
        start_kwallet();
    } else {
        unsetenv("KDE_SESSION_VERSION");
        start_gnome_keyring();
    }
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
