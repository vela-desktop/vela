// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// A locker that has sent unlock_and_destroy still owns its process group
// during cleanup. A subsequent launch must have independent supervision.
#undef NDEBUG
#include <assert.h>
#include "../src/lock.c"

static void assert_reaped(pid_t child)
{
    assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
}

int main(void)
{
    unsetenv("VELA_SCREEN_OFF");
    struct vela_server server = { .display = wl_display_create(), .runtime_dir_fd = -1 };
    assert(server.display);
    server.loop = wl_display_get_event_loop(server.display);
    server.scene = vela_scene_create();
    server.layers.lock = vela_tree_create(server.scene->root);
    struct vela_lock *lock = vela_lock_create(&server);

    lock_start_launcher(lock, "exit 19");
    pid_t clean = lock->launcher_pid;
    assert(clean > 0 && lock->locker_client);
    handle_lock_destroy(&lock->destroy, NULL);
    assert(!lock->launcher_pid && !lock->locker_client);
    // Observe the actual exit code without consuming the production reaper's child.
    siginfo_t info = { 0 };
    assert(waitid(P_PID, clean, &info, WEXITED | WNOWAIT) == 0);
    assert(info.si_code == CLD_EXITED && info.si_status == 19);

    lock_start_launcher(lock, "exec sleep 30");
    pid_t next = lock->launcher_pid;
    assert(next > 0 && next != clean);
    while (!wl_list_empty(&lock->exiting_launchers)) {
        assert(wl_event_loop_dispatch(server.loop, -1) == 0);
    }
    assert_reaped(clean);
    assert(lock->launcher_pid == next && waitpid(next, NULL, WNOHANG) == 0);

    // The second locker stays alive after unlock. Its grace timer must reap
    // it too, while the display and command loop continue running.
    handle_lock_destroy(&lock->destroy, NULL);
    while (!wl_list_empty(&lock->exiting_launchers)) {
        assert(wl_event_loop_dispatch(server.loop, -1) == 0);
    }
    assert_reaped(next);

    // Shutdown while another launcher is still in its grace period.
    lock_start_launcher(lock, "exec sleep 30");
    pid_t last = lock->launcher_pid;
    assert(last > 0);
    handle_lock_destroy(&lock->destroy, NULL);
    vela_lock_destroy(lock);
    assert_reaped(last);
    wl_display_destroy_clients(server.display);
    vela_node_destroy(&server.layers.lock->node);
    vela_scene_destroy(server.scene);
    wl_display_destroy(server.display);
    return 0;
}
