// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_WAIT_ARRAYS_H
#define VELA_WAIT_ARRAYS_H

#include <vulkan/vulkan.h>

// Reusable arrays for Vulkan semaphore submissions, independently sized.
// On OOM vela_grow terminates explicitly instead of losing a live pointer.
void vela_wait_arrays_reserve(VkSemaphoreSubmitInfo **infos, int *infos_capacity,
    VkSemaphore **semaphores, int *semaphores_capacity, int count);

#endif
