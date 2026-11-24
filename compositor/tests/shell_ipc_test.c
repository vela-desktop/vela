// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the production focus selection and wire protocol without a GPU.
#include "focus.h"
#include "layer.h"
#include "output.h"
#include "server.h"
#include "shell.h"
#include "view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/types/wlr_output.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static struct vela_view view;
static struct vela_layer_surface layer;
static struct vela_view *focused;
static struct vela_output *window_output, *layer_output, *pointer_output;

struct vela_view *vela_views_focused(struct vela_server *server)
{
    (void)server;
    return focused;
}

struct vela_output *vela_view_output(struct vela_view *v)
{
    CHECK(v == &view);
    return window_output;
}

struct vela_output *vela_layer_surface_output(const struct vela_layer_surface *l)
{
    CHECK(l == &layer);
    return layer_output;
}

struct vela_output *vela_output_under_cursor(const struct vela_server *server)
{
    (void)server;
    return pointer_output;
}

static int listener(const struct sockaddr_un *addr)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    CHECK(bind(fd, (const struct sockaddr *)addr, sizeof(*addr)) == 0);
    CHECK(listen(fd, 4) == 0);
    return fd;
}

static void expect_command(struct vela_server *server, int fd, const char *expected)
{
    CHECK(vela_shell_send(server, "volume up"));
    int peer = accept(fd, NULL, NULL);
    CHECK(peer >= 0);
    char buf[256];
    size_t used = 0;
    ssize_t n;
    while ((n = read(peer, buf + used, sizeof(buf) - used - 1)) > 0) {
        used += (size_t)n;
        CHECK(used < sizeof(buf) - 1);
    }
    CHECK(n == 0);
    buf[used] = '\0';
    CHECK(strcmp(buf, expected) == 0);
    close(peer);
}

int main(void)
{
    struct vela_server *server = calloc(1, sizeof(*server));
    CHECK(server);
    struct wlr_output first_wlr = { .name = "DP-1" }, second_wlr = { .name = "DP-2" };
    struct vela_output first = { .wlr = &first_wlr }, second = { .wlr = &second_wlr };
    char directory[] = "/tmp/vela-shell-ipc-XXXXXX";
    CHECK(mkdtemp(directory));
    CHECK(setenv("XDG_RUNTIME_DIR", directory, 1) == 0);
    snprintf(server->socket_name, sizeof(server->socket_name), "test");
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/vela-shell-test.sock", directory);
    int fd = listener(&addr);

    pointer_output = &first;
    expect_command(server, fd, "active-output DP-1\nvolume up\n");
    focused = &view;
    window_output = &second;
    expect_command(server, fd, "active-output DP-2\nvolume up\n");
    expect_command(server, fd, "active-output DP-2\nvolume up\n");

    // Failed delivery and a replacement listener on the same display.
    close(fd);
    CHECK(unlink(addr.sun_path) == 0);
    CHECK(!vela_shell_send(server, "volume up"));
    fd = listener(&addr);
    expect_command(server, fd, "active-output DP-2\nvolume up\n");

    focused = NULL;
    server->focused_layer = &layer;
    layer_output = &second;
    expect_command(server, fd, "active-output DP-2\nvolume up\n");
    layer_output = NULL;
    expect_command(server, fd, "active-output DP-1\nvolume up\n");
    server->focused_layer = NULL;
    pointer_output = NULL;
    expect_command(server, fd, "active-output \nvolume up\n");

    close(fd);
    CHECK(unlink(addr.sun_path) == 0);
    CHECK(rmdir(directory) == 0);
    free(server);
    return 0;
}
