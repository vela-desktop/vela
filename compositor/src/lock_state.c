// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lock_state.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOCK_STATE_UNLOCKED 0u

struct vela_lock_state {
    atomic_uint value;
};

static const unsigned required_seals = F_SEAL_SHRINK | F_SEAL_GROW;

static long page_size(void)
{
    long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? size : 4096;
}

int vela_lock_state_create(void)
{
    int fd = memfd_create("vela-lock-state", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        return -1;
    }
    // Write the initial value instead of only truncating: the page is then
    // allocated here, where a failure is an ordinary error, and never on the
    // compositor's first store.
    const unsigned initial = LOCK_STATE_UNLOCKED;
    if (ftruncate(fd, page_size()) != 0
        || pwrite(fd, &initial, sizeof(initial), 0) != (ssize_t)sizeof(initial)
        || fcntl(fd, F_ADD_SEALS, required_seals | F_SEAL_SEAL) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

int vela_lock_state_parse_fd(const char *text)
{
    if (!text || *text < '0' || *text > '9') {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end || value < 0 || value > INT_MAX) {
        return -1;
    }
    return (int)value;
}

struct vela_lock_state *vela_lock_state_map(int fd)
{
    if (fd < 0) {
        return NULL;
    }
    // Only a sealed memfd of exactly one page: anything else could be
    // truncated under the mapping (SIGBUS) or is not the supervisor's page.
    struct stat info;
    int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || ((unsigned)seals & required_seals) != required_seals
        || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)
        || info.st_size != (off_t)page_size()) {
        return NULL;
    }
    void *page = mmap(NULL, (size_t)page_size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (page == MAP_FAILED) {
        return NULL;
    }
    // The mapping keeps the page alive; without a descriptor no child can
    // inherit a writable view of it.
    close(fd);
    return page;
}

void vela_lock_state_unmap(struct vela_lock_state *state)
{
    if (state) {
        munmap(state, (size_t)page_size());
    }
}

bool vela_lock_state_locked(const struct vela_lock_state *state)
{
    if (!state) {
        return false;
    }
    return atomic_load_explicit(&((struct vela_lock_state *)state)->value, memory_order_acquire)
        != LOCK_STATE_UNLOCKED;
}

void vela_lock_state_store(struct vela_lock_state *state, bool locked)
{
    if (state) {
        atomic_store_explicit(&state->value, locked ? 1u : LOCK_STATE_UNLOCKED, memory_order_release);
    }
}

bool vela_lock_start_locked(bool page_locked, enum vela_lock_marker marker)
{
    return page_locked || marker != VELA_LOCK_MARKER_ABSENT;
}

bool vela_lock_restart_locked(const struct vela_lock_state *state, int runtime_fd, const char *display,
    bool forced, bool *unreadable)
{
    *unreadable = false;
    if (!state) {
        return forced;
    }
    enum vela_lock_marker marker = vela_lock_marker_state_at(runtime_fd, display);
    *unreadable = marker == VELA_LOCK_MARKER_UNKNOWN;
    return forced || vela_lock_start_locked(vela_lock_state_locked(state), marker);
}
