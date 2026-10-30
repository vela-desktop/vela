// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/device_loss_policy.h"
#include "session.h"
#undef NDEBUG
#include <assert.h>
#include <string.h>
int main(void)
{
    struct vela_device_loss_policy state = { 0 };
    assert(!vela_device_loss_mark(&state, 0, "success"));
    assert(!vela_device_loss_mark(&state, -1, "host OOM"));
    assert(!vela_device_loss_mark(&state, -2, "device OOM"));
    assert(!state.lost);
    assert(vela_device_loss_mark(&state, -4, "vkQueueSubmit2"));
    assert(state.lost && state.first_result == -4);
    assert(strcmp(state.first_operation, "vkQueueSubmit2") == 0);
    assert(!vela_device_loss_mark(&state, -4, "vkWaitSemaphores"));
    assert(!vela_device_loss_mark(&state, 0, "later success"));
    assert(strcmp(state.first_operation, "vkQueueSubmit2") == 0);
    // The supervisor owns the last stop hook across a terminal restart;
    // direct sessions must not leave their systemd session target active.
    for (int session = 0; session < 2; ++session) {
        for (int supervised = 0; supervised < 2; ++supervised) {
            for (int terminal = 0; terminal < 2; ++terminal) {
                bool expected = session && (!supervised || !terminal);
                assert(vela_session_should_stop(session, supervised, terminal) == expected);
            }
        }
    }
    return 0;
}
