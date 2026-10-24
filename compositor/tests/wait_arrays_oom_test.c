// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the renderer's real wait-array allocation helper without a GPU.
// Linker --wrap=realloc fails the first or second allocation on demand.
#include "render/wait_arrays.h"

#undef NDEBUG // CI uses RelWithDebInfo: assertions must remain active.
#include <assert.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int allocation_count;
static int fail_allocation;

void *__real_realloc(void *ptr, size_t size);
void *__wrap_realloc(void *ptr, size_t size)
{
    if (++allocation_count == fail_allocation) {
        return NULL;
    }
    return __real_realloc(ptr, size);
}

static void successful_growth(void)
{
    VkSemaphoreSubmitInfo *infos = NULL;
    VkSemaphore *semaphores = NULL;
    int infos_capacity = 0, semaphores_capacity = 0;
    allocation_count = 0;
    fail_allocation = -1;

    vela_wait_arrays_reserve(&infos, &infos_capacity, &semaphores, &semaphores_capacity, 2);
    assert(infos && semaphores && infos_capacity >= 2 && semaphores_capacity >= 2);
    assert(allocation_count == 2);
    infos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    semaphores[0] = (VkSemaphore)(uintptr_t)1;
    VkSemaphoreSubmitInfo *old_infos = infos;
    VkSemaphore *old_semaphores = semaphores;

    // Repeated and empty submissions must not realloc.
    vela_wait_arrays_reserve(&infos, &infos_capacity, &semaphores, &semaphores_capacity, 0);
    vela_wait_arrays_reserve(&infos, &infos_capacity, &semaphores, &semaphores_capacity, 2);
    assert(allocation_count == 2 && infos == old_infos && semaphores == old_semaphores);
    assert(infos[0].sType == VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    assert(semaphores[0] == (VkSemaphore)(uintptr_t)1);

    // When capacities differ, only the smaller advertised array grows.
    semaphores_capacity = 1;
    vela_wait_arrays_reserve(&infos, &infos_capacity, &semaphores, &semaphores_capacity, 2);
    assert(allocation_count == 3);
    assert(infos == old_infos && infos_capacity >= 2 && semaphores_capacity >= 2);
    free(infos);
    free(semaphores);
}

static void expect_abort(int nth_failure)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        VkSemaphoreSubmitInfo *infos = NULL;
        VkSemaphore *semaphores = NULL;
        int infos_capacity = 0, semaphores_capacity = 0;
        allocation_count = 0;
        fail_allocation = nth_failure;
        vela_wait_arrays_reserve(&infos, &infos_capacity, &semaphores, &semaphores_capacity, 20);
        _exit(77); // OOM must terminate instead of returning an invalid array
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

int main(void)
{
    struct rlimit no_core = { 0, 0 };
    assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
    successful_growth();
    expect_abort(1);
    expect_abort(2);
    return 0;
}
