// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#undef NDEBUG
#include "a11y.h"
#include "buffer.h"
#include "command.h"
#include "input.h"
#include "lock.h"
#include "scene/scene.h"
#include "server.h"
#include "workspace.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/types/wlr_seat.h>

static unsigned table_reads;
static struct sockaddr_un address = { .sun_family = AF_UNIX };

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode)
{
    static char primary[] = "# zone fixture\nIT +4154+01229 Europe/Rome\n";
    static char fallback[] = "AU -3352+15113 Australia/Sydney\n";
    if (strcmp(path, "/usr/share/zoneinfo/zone1970.tab") == 0) {
        ++table_reads;
        return fmemopen(primary, sizeof(primary) - 1, "r");
    }
    if (strcmp(path, "/usr/share/zoneinfo/zone.tab") == 0) {
        ++table_reads;
        return fmemopen(fallback, sizeof(fallback) - 1, "r");
    }
    return __real_fopen(path, mode);
}

// Make a modest pipeline fill the real socket regardless of host defaults.
int __real_accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
int __wrap_accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    int client = __real_accept4(fd, addr, len, flags);
    if (client >= 0) {
        int size = 4096;
        assert(setsockopt(client, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
    }
    return client;
}

static int connect_client(struct wl_event_loop *loop)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(wl_event_loop_dispatch(loop, 0) == 0);
    return fd;
}

static void request(int fd, const void *bytes, size_t length)
{
    assert(send(fd, bytes, length, MSG_NOSIGNAL) == (ssize_t)length);
}

// Read until the server closes its half of the socket. Keep dispatching
// while it waits for room to send a partial line or accepts another batch.
static void receive(struct wl_event_loop *loop, int fd, struct vela_buffer *reply)
{
    assert(shutdown(fd, SHUT_WR) == 0);
    for (;;) {
        assert(wl_event_loop_dispatch(loop, 0) == 0);
        char chunk[1024];
        ssize_t n = recv(fd, chunk, sizeof(chunk) - 1, MSG_DONTWAIT);
        if (!n) {
            break;
        }
        if (n < 0) {
            assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
            continue;
        }
        chunk[n] = '\0';
        vela_buffer_append(reply, chunk);
    }
    close(fd);
}

static void pipelined_replies(struct vela_server *server)
{
    char state[512];
    assert(vela_a11y_json(server->a11y, state, sizeof(state)));
    assert(strstr(state, "\"sunrise\":") && strstr(state, "\"sunset\":"));
    unsigned before = table_reads;
    struct vela_buffer expected = { 0 }, replies = { 0 }, commands = { 0 };
    // Start with a single reply larger than SO_SNDBUF to force a short
    // write within a line as well as EAGAIN between pipelined questions.
    char name[201];
    memset(name, 'w', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *names[96];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        names[i] = name;
    }
    struct vela_workspaces ws = { .names = names, .count = 96 };
    server->workspaces = &ws;
    vela_buffer_append(&commands, "workspaces\n");
    vela_workspaces_json(server, &expected);
    vela_buffer_append(&expected, "\n");
    for (int i = 0; i < 192; ++i) {
        vela_buffer_append(&commands, "accessibility\ncaffeine status\n");
        vela_buffer_appendf(&expected, "%s\noff\n", state);
    }
    int slow = connect_client(server->loop);
    request(slow, commands.data, commands.length);
    assert(wl_event_loop_dispatch(server->loop, 0) == 0);

    // The first client is stalled. A second one still gets complete lines,
    // even when the beginning of the next command came in a previous read.
    int fast = connect_client(server->loop);
    const char first[] = "watch capture\nignored\0caffeine on\ncaff";
    request(fast, first, sizeof(first) - 1);
    assert(wl_event_loop_dispatch(server->loop, 0) == 0);
    const char rest[] = "eine status\n";
    request(fast, rest, sizeof(rest) - 1);
    receive(server->loop, fast, &replies);
    assert(strcmp(replies.data, "{\"clients\":[]}\noff\n") == 0);
    vela_buffer_finish(&replies);

    receive(server->loop, slow, &replies);
    assert(strcmp(replies.data, expected.data) == 0);
    assert(table_reads == before);
    vela_buffer_finish(&replies);
    vela_buffer_finish(&expected);
    vela_buffer_finish(&commands);
    server->workspaces = NULL;
}

static void check_caffeine(struct vela_server *server, const char *line, bool ok, bool active)
{
    int fd = connect_client(server->loop);
    request(fd, line, strlen(line));
    struct vela_buffer reply = { 0 };
    receive(server->loop, fd, &reply);
    assert(strstr(reply.data, ok ? "\"ok\":true" : "\"ok\":false"));
    assert(strstr(reply.data, active ? "\"caffeine\":true" : "\"caffeine\":false"));
    vela_buffer_finish(&reply);
}

static void caffeine_expiry(struct vela_server *server)
{
    check_caffeine(server, "caffeine-set on\n", true, true);
    assert(vela_lock_caffeine_remaining_s(server->lock) == -1);
    check_caffeine(server, "caffeine-set on 1441\n", false, true);
    check_caffeine(server, "caffeine-set on nan\n", false, true);
    check_caffeine(server, "caffeine-set toggle\n", true, false);
    check_caffeine(server, "caffeine-set on 0.001\n", true, true);
    while (server->lock->caffeine) {
        assert(wl_event_loop_dispatch(server->loop, -1) == 0);
    }
    assert(vela_lock_caffeine_remaining_s(server->lock) == 0);
}

static void refresh_on_timer(struct vela_server *server)
{
    assert(wl_event_source_timer_update(server->a11y->timer, 1) == 0);
    assert(wl_event_loop_dispatch(server->loop, -1) == 0);
}

static void timezone_changes(struct vela_server *server)
{
    struct vela_a11y *a11y = server->a11y;
    unsigned before = table_reads;
    refresh_on_timer(server);
    assert(table_reads == before);
    assert(setenv("TZ", ":Australia/Sydney", 1) == 0);
    refresh_on_timer(server);
    assert(a11y->has_coordinates && a11y->latitude < 0 && a11y->longitude > 150);
    assert(table_reads == before + 2);

    assert(setenv("TZ", "Etc/UTC", 1) == 0);
    refresh_on_timer(server);
    assert(!a11y->has_coordinates && table_reads == before + 4);
    char state[512];
    assert(vela_a11y_json(a11y, state, sizeof(state)) && !strstr(state, "\"sunrise\""));
    refresh_on_timer(server);
    assert(table_reads == before + 4); // missing coordinates are cached too
    vela_a11y_load(a11y);
    assert(table_reads == before + 6); // config reload retries the tables
}

int main(void)
{
    // Keep the socket path short even when CTest runs in a deeply nested build directory.
    char directory[] = "/proc/self/cwd/vela-command-test-XXXXXX";
    assert(mkdtemp(directory));
    assert(setenv("XDG_RUNTIME_DIR", directory, 1) == 0);
    assert(setenv("XDG_CONFIG_HOME", directory, 1) == 0);
    assert(setenv("TZ", "Europe/Rome", 1) == 0);
    unsetenv("VELA_TEST_COMMANDS");
    unsetenv("VELA_SCREEN_OFF");

    struct vela_input input = { 0 };
    wl_list_init(&input.pointers);
    struct vela_server server = { .display = wl_display_create(), .input = &input, .socket_name = "test" };
    assert(server.display);
    server.loop = wl_display_get_event_loop(server.display);
    server.seat = wlr_seat_create(server.display, "test");
    assert(server.seat);
    wl_list_init(&server.outputs);
    wl_list_init(&server.views);
    server.scene = vela_scene_create();
    server.layers.lock = vela_tree_create(server.scene->root);
    server.lock = vela_lock_create(&server);
    server.a11y = vela_a11y_create(&server);
    vela_a11y_load(server.a11y);
    assert(server.a11y->has_coordinates && table_reads == 1);
    vela_commands_listen(&server);
    snprintf(address.sun_path, sizeof(address.sun_path), "%s/vela-test.sock", directory);
    pipelined_replies(&server);
    timezone_changes(&server);
    caffeine_expiry(&server);

    vela_commands_stop(&server);
    assert(access(address.sun_path, F_OK) < 0 && errno == ENOENT);
    vela_a11y_destroy(server.a11y);
    vela_lock_destroy(server.lock);
    vela_node_destroy(&server.layers.lock->node);
    vela_scene_destroy(server.scene);
    wl_display_destroy(server.display);
    assert(rmdir(directory) == 0);
    return 0;
}
