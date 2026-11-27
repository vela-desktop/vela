// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Whether the test runs under Valgrind (the Memcheck workflow,
// docs/ci-lanes.md), for the few checks Valgrind cannot emulate: its own
// operator new, malloc and descriptor limit replace the program's. C and C++.
// Without Valgrind's headers, as in every other build, never.

#pragma once

#if defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define VELA_UNDER_VALGRIND() (RUNNING_ON_VALGRIND != 0)
#endif
#endif

#ifndef VELA_UNDER_VALGRIND
#define VELA_UNDER_VALGRIND() 0
#endif
