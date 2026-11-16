// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_TESTS_ALLOC_COUNT_H
#define VELA_TESTS_ALLOC_COUNT_H

// A count of the calls to Vela's allocation helpers (common/alloc), for the
// frame-path tests (docs/memory-allocation-policy.md, "Frame path"). It is
// linked only into test binaries, with
//   -Wl,--wrap=vela_alloc_at -Wl,--wrap=vela_grow_size_at
//   -Wl,--wrap=vela_grow_int_at -Wl,--wrap=vela_strdup_at
//   -Wl,--wrap=vela_strndup_at -Wl,--wrap=vela_asprintf_at
// so every call from Vela's own objects passes through it.
//
// An allocation is a call that obtains a block: vela_alloc, the string
// copies, vela_asprintf, and a vela_grow that grows the array. A vela_grow
// that finds room is only a comparison; it is counted apart, as a check.
//
// With VELA_TEST_ALLOC_LOG_FD=<n> in the environment, n being a descriptor
// the test opened for appending and the process inherited, each allocation
// also appends one line to it, "<kind> <file>:<line> <count>x<size>", with
// write(2), so a test can read which sites allocated over a window of time.

#include <stdint.h>

struct vela_alloc_count {
    uint64_t allocations; // blocks obtained
    uint64_t checks; // vela_grow calls that found room
};

struct vela_alloc_count vela_alloc_count_get(void);

#endif
