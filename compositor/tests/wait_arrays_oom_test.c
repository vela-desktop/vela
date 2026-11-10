// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the renderer's real wait-array allocation helper without a GPU.
// Linker --wrap=realloc fails the first or second allocation under vela_grow
// (common/alloc) on demand.
#include "render/wait_arrays.h"

#undef NDEBUG // CI uses RelWithDebInfo: assertions must remain active.
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

// The nth realloc fails: the process must abort through vela_oom, whose line
// names wait_arrays.c, instead of returning an invalid array.
static void expect_abort(int nth_failure, const char *expected)
{
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDERR_FILENO);
        VkSemaphoreSubmitInfo *infos = NULL;
        VkSemaphore *semaphores = NULL;
        int infos_capacity = 0, semaphores_capacity = 0;
        allocation_count = 0;
        fail_allocation = nth_failure;
        vela_wait_arrays_reserve(&infos, &infos_capacity, &semaphores, &semaphores_capacity, 20);
        _exit(77);
    }
    close(pipe_fds[1]);
    char text[512];
    size_t length = 0;
    while (length < sizeof(text) - 1) {
        ssize_t n = read(pipe_fds[0], text + length, sizeof(text) - 1 - length);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        length += (size_t)n;
    }
    text[length] = '\0';
    close(pipe_fds[0]);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(strstr(text, expected) && strstr(text, "render/wait_arrays.c:"));
}

int main(void)
{
    struct rlimit no_core = { 0, 0 };
    assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
    successful_growth();
    // 20 entries: capacity 32. The first realloc is the infos array, the
    // second the semaphores.
    char infos[64], semaphores[64];
    snprintf(infos, sizeof(infos), "vela: out of memory (32 x %zu bytes)", sizeof(VkSemaphoreSubmitInfo));
    snprintf(semaphores, sizeof(semaphores), "vela: out of memory (32 x %zu bytes)", sizeof(VkSemaphore));
    expect_abort(1, infos);
    expect_abort(2, semaphores);
    return 0;
}
