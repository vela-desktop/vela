// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VELA_RENDER_DEVICE_LOSS_POLICY_H
#define VELA_RENDER_DEVICE_LOSS_POLICY_H
#include <stdbool.h>
#include <stdint.h>
// Vulkan VK_ERROR_DEVICE_LOST has VkResult value -4.
#define VELA_VK_DEVICE_LOST_RESULT (-4)
struct vela_device_loss_policy {
    bool lost;
    int32_t first_result;
    const char *first_operation;
};
static inline bool vela_device_loss_mark(struct vela_device_loss_policy *state, int32_t result,
    const char *operation)
{
    if (state->lost || result != VELA_VK_DEVICE_LOST_RESULT) return false;
    state->lost = true;
    state->first_result = result;
    state->first_operation = operation;
    return true;
}
#endif
