// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Which secret store the session selects, with a private HOME. No compositor.

#include "session.h"
#include "supervisor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

bool vela_session_hook_path(char *out, size_t size)
{
    (void)out;
    (void)size;
    return false;
}

static void expect(const char *name, const char *want)
{
    const char *got = getenv(name);
    if (!got) {
        got = "";
    }
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s is \"%s\", expected \"%s\"\n", name, got, want);
        failures++;
    }
}

static void write_file(const char *path, size_t bytes)
{
    FILE *file = fopen(path, "w");
    if (!file) {
        perror(path);
        exit(1);
    }
    for (size_t i = 0; i < bytes; ++i) {
        fputc('x', file);
    }
    fclose(file);
}

int main(void)
{
    char home[] = "/tmp/vela-session-env-XXXXXX";
    if (!mkdtemp(home)) {
        return 1;
    }
    setenv("HOME", home, 1);
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);

    vela_session_set_environment();
    expect("XDG_CURRENT_DESKTOP", "Vela:KDE");
    expect("KDE_SESSION_VERSION", "6");

    char path[512];
    snprintf(path, sizeof(path), "%s/.local/share/kwalletd", home);
    if (mkdir(path, 0700) != 0 && mkdir(home, 0700) != 0) {
        /* parents created below */
    }
    snprintf(path, sizeof(path), "%s/.local", home);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/.local/share", home);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/.local/share/kwalletd", home);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/.local/share/keyrings", home);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/.local/share/kwalletd/Default.kwl", home);
    write_file(path, 40000);
    vela_session_set_environment();
    expect("XDG_CURRENT_DESKTOP", "Vela:KDE");

    snprintf(path, sizeof(path), "%s/.local/share/keyrings/login.keyring", home);
    write_file(path, 56000);
    unsetenv("KDE_SESSION_VERSION");
    unsetenv("SSH_AUTH_SOCK");
    vela_session_set_environment();
    expect("XDG_CURRENT_DESKTOP", "Vela:GNOME");
    expect("KDE_SESSION_VERSION", "");
    expect("SSH_AUTH_SOCK", "/run/user/1000/keyring/ssh");
    expect("GNOME_KEYRING_CONTROL", "/run/user/1000/keyring");
    return failures == 0 ? 0 : 1;
}
