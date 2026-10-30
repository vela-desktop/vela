// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Satellite X11 backend is opt-in in vela.conf (xwayland-backend=native).
// Unlike per-game DISPLAY wrappers, all processes launched in this Vela session
// inherit this DISPLAY, including Steam and games. Built-in XWayland remains
// available as a compatibility fallback.
//
// Vela owns both the filesystem and abstract X11 listen sockets. Passing them
// with -listenfd means Steam can connect before Satellite finishes the first
// Wayland round trip, and the sockets survive Satellite restarts.
#include "x11_satellite.h"

#include "config.h"
#include "server.h"
#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/util/log.h>

static bool in_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !*path) {
        return false;
    }
    while (*path) {
        const char *end = strchr(path, ':');
        if (!end) {
            end = path + strlen(path);
        }
        char candidate[512];
        int n = path == end ? snprintf(candidate, sizeof(candidate), "./%s", name)
            : snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)(end - path), path, name);
        if (n > 0 && (size_t)n < sizeof(candidate) && access(candidate, X_OK) == 0) {
            return true;
        }
        path = *end ? end + 1 : end;
    }
    return false;
}

static int bind_socket(struct sockaddr_un *address, socklen_t length)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (bind(fd, (struct sockaddr *)address, length) != 0 || listen(fd, 64) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool vela_x11_satellite_start(struct vela_server *server)
{
    struct vela_config config;
    vela_config_read(&config);
    bool native = strcmp(vela_config_get(&config, "xwayland-backend", "compat"), "native") == 0;
    vela_config_finish(&config);
    if (!native) {
        return false;
    }
    if (!in_path("xwayland-satellite")) {
        wlr_log(WLR_ERROR, "Native XWayland requested but xwayland-satellite is not installed; using built-in XWayland");
        return false;
    }

    // Built-in wlroots XWayland already owns its display. Do not replace or
    // touch those sockets: it remains available for compatibility.
    for (int number = 80; number < 200; ++number) {
        char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
        snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", number);
        char lock[64];
        snprintf(lock, sizeof(lock), "/tmp/.X%d-lock", number);
        if (access(path, F_OK) == 0 || access(lock, F_OK) == 0) {
            continue;
        }

        struct sockaddr_un abstract_addr = { .sun_family = AF_UNIX };
        size_t name_length = strlen(path);
        if (name_length + 1 >= sizeof(abstract_addr.sun_path)) {
            continue;
        }
        // Linux abstract UNIX socket: Xlib and Wine try it first.
        memcpy(abstract_addr.sun_path + 1, path, name_length);
        int abstract_fd = bind_socket(&abstract_addr,
            (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_length));
        if (abstract_fd < 0) {
            continue;
        }

        struct sockaddr_un file_addr = { .sun_family = AF_UNIX };
        snprintf(file_addr.sun_path, sizeof(file_addr.sun_path), "%s", path);
        int file_fd = bind_socket(&file_addr,
            (socklen_t)(offsetof(struct sockaddr_un, sun_path) + name_length + 1));
        if (file_fd < 0) {
            close(abstract_fd);
            continue;
        }

        char command[256];
        snprintf(command, sizeof(command), "exec xwayland-satellite :%d -listenfd %d -listenfd %d",
            number, abstract_fd, file_fd);
        // The existing child supervisor restarts Satellite after a failure,
        // reusing the same socket FDs so DISPLAY never changes underneath apps.
        server->satellite.keep_fd = abstract_fd;
        server->satellite.keep_fd2 = file_fd;
        server->satellite_listen_fd = file_fd;
        if (!vela_child_start(&server->satellite, server->loop, command)) {
            close(file_fd);
            close(abstract_fd);
            unlink(path);
            server->satellite_listen_fd = -1;
            server->satellite.keep_fd = -1;
            server->satellite.keep_fd2 = -1;
            return false;
        }
        server->satellite_display = number;
        snprintf(server->satellite_socket_path, sizeof(server->satellite_socket_path), "%s", path);
        wlr_log(WLR_INFO, "Native-resolution X11 Satellite listening on DISPLAY=:%d", number);
        return true;
    }
    wlr_log(WLR_ERROR, "Unable to reserve an X11 display for Satellite; using built-in XWayland");
    return false;
}

void vela_x11_satellite_stop(struct vela_server *server)
{
    if (server->satellite.pid > 0) {
        kill(server->satellite.pid, SIGTERM);
    }
    vela_child_stop(&server->satellite);
    if (server->satellite.keep_fd >= 0) {
        close(server->satellite.keep_fd);
        server->satellite.keep_fd = -1;
    }
    if (server->satellite_listen_fd >= 0) {
        close(server->satellite_listen_fd);
        server->satellite_listen_fd = -1;
        server->satellite.keep_fd2 = -1;
    }
    if (server->satellite_socket_path[0]) {
        unlink(server->satellite_socket_path);
        server->satellite_socket_path[0] = 0;
    }
}
