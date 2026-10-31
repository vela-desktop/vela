// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Satellite is integrated as an experimental X11 backend (xwayland-backend=native).
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
#include <sys/random.h>
#include <sys/wait.h>
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

// Xauthority wire format: big-endian 16-bit length-prefixed fields.
 // FamilyWild (65535) authenticates both UNIX and Linux abstract connections.
static bool auth_write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *bytes = buffer;
    while (size) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n;
        size -= (size_t)n;
    }
    return true;
}

static bool auth_field(int fd, const void *value, size_t size)
{
    if (size > UINT16_MAX) return false;
    const unsigned char length[] = { (unsigned char)(size >> 8), (unsigned char)size };
    return auth_write_all(fd, length, sizeof(length))
        && (!size || auth_write_all(fd, value, size));
}

static bool create_xauthority(char *path, size_t capacity, int display)
{
    static const char name[] = "MIT-MAGIC-COOKIE-1";
    unsigned char cookie[16];
    size_t filled = 0;
    while (filled < sizeof(cookie)) {
        ssize_t n = getrandom(cookie + filled, sizeof(cookie) - filled, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        filled += (size_t)n;
    }

    char template[] = "/tmp/vela-x11-auth-XXXXXX";
    int fd = mkstemp(template); // mode 0600; never follow symlinks
    if (fd < 0) return false;
    char number[16];
    snprintf(number, sizeof(number), "%d", display);
    const unsigned char family_wild[] = { 0xff, 0xff };
    bool ok = auth_write_all(fd, family_wild, sizeof(family_wild))
        && auth_field(fd, NULL, 0) // address: any transport
        && auth_field(fd, number, strlen(number))
        && auth_field(fd, name, sizeof(name) - 1)
        && auth_field(fd, cookie, sizeof(cookie));
    if (close(fd) < 0) ok = false;
    if (!ok || strlen(template) >= capacity) {
        unlink(template);
        return false;
    }
    strcpy(path, template);
    return true;
}

static void restore_xauthority(struct vela_server *server)
{
    if (server->satellite_auth_path[0]) {
        unlink(server->satellite_auth_path);
        server->satellite_auth_path[0] = 0;
    }
    if (server->satellite_previous_xauthority) {
        setenv("XAUTHORITY", server->satellite_previous_xauthority, 1);
        free(server->satellite_previous_xauthority);
        server->satellite_previous_xauthority = NULL;
    } else {
        unsetenv("XAUTHORITY");
    }
}

// A binary found in PATH is not necessarily compatible with -listenfd.
static bool satellite_supports_listenfd(void)
{
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        execlp("xwayland-satellite", "xwayland-satellite", ":0",
            "--test-listenfd-support", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    return result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
    // Native-resolution X11 is the Vela default. Allow a session override for
    // CI, debugging and recovery without editing the user's config file.
    const char *backend = getenv("VELA_XWAYLAND_BACKEND");
    if (!backend || !*backend) {
        backend = vela_config_get(&config, "xwayland-backend", "compat");
    }
    bool native = strcmp(backend, "native") == 0;
    bool valid = native || strcmp(backend, "compat") == 0;
    if (!valid) {
        wlr_log(WLR_ERROR, "Unknown xwayland-backend '%s'; using compat", backend);
    }
    vela_config_finish(&config);
    if (!native) return false;
    if (!in_path("xwayland-satellite")) {
        wlr_log(WLR_ERROR, "Native XWayland requested but xwayland-satellite is not installed; using built-in XWayland");
        return false;
    }

    if (!satellite_supports_listenfd()) {
        wlr_log(WLR_ERROR, "xwayland-satellite does not support -listenfd; using built-in XWayland");
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

        // Keep the filesystem socket private and require the same random cookie
        // even for abstract UNIX socket clients (which cannot use filesystem ACLs).
        if (chmod(path, 0600) < 0
            || !create_xauthority(server->satellite_auth_path,
                sizeof(server->satellite_auth_path), number)) {
            wlr_log(WLR_ERROR, "Unable to secure Satellite X11 socket/authentication");
            close(file_fd);
            close(abstract_fd);
            unlink(path);
            return false;
        }
        const char *old_auth = getenv("XAUTHORITY");
        server->satellite_previous_xauthority = old_auth ? strdup(old_auth) : NULL;
        if ((old_auth && !server->satellite_previous_xauthority)
            || setenv("XAUTHORITY", server->satellite_auth_path, 1) != 0) {
            // setenv failed (or old value could not be preserved).
            if (!server->satellite_previous_xauthority) {
                unlink(server->satellite_auth_path);
                server->satellite_auth_path[0] = 0;
            } else {
                restore_xauthority(server);
            }
            close(file_fd);
            close(abstract_fd);
            unlink(path);
            return false;
        }
        char command[320];
        snprintf(command, sizeof(command),
            "exec xwayland-satellite :%d -auth %s -listenfd %d -listenfd %d",
            number, server->satellite_auth_path, abstract_fd, file_fd);
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
            restore_xauthority(server);
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
    if (server->satellite_auth_path[0]) restore_xauthority(server);
}
