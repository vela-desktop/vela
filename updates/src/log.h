// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_UP_LOG_H
#define VELA_UP_LOG_H

// The daemon's log, on stderr (the compositor's log, or the journal).

#include <stdio.h>

#define vela_up_log(...)                                                                                               \
    do {                                                                                                               \
        fprintf(stderr, "vela-update-daemon: " __VA_ARGS__);                                                           \
        fputc('\n', stderr);                                                                                           \
    } while (0)

#endif
