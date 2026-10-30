// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/device_loss_policy.h"
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
    return 0;
}
