// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/wait_arrays.h"
#include "util.h"

void vela_wait_arrays_reserve(VkSemaphoreSubmitInfo **infos, int *infos_capacity,
    VkSemaphore **semaphores, int *semaphores_capacity, int count)
{
    *infos = vela_grow(*infos, infos_capacity, count, sizeof(**infos));
    *semaphores = vela_grow(*semaphores, semaphores_capacity, count, sizeof(**semaphores));
}
