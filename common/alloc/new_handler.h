// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_NEW_HANDLER_H
#define VELA_NEW_HANDLER_H

// The C++ processes' answer to a failed operator new
// (docs/memory-allocation-policy.md, "Scope"): the same one-line report as
// the C helpers, then abort(), and the supervisor restarts what it watches.
// Installed first thing in main:
//   vela::install_new_handler("vela-shell");

#include "alloc/alloc.h"

#include <new>

namespace vela {

inline const char *new_handler_process = "?";

[[noreturn]] inline void report_failed_new()
{
    vela_new_failed(new_handler_process);
}

inline void install_new_handler(const char *process)
{
    new_handler_process = process;
    std::set_new_handler(report_failed_new);
}

} // namespace vela

#endif
