// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_OUTPUT_KEY_H
#define VELA_OUTPUT_KEY_H

#include <stddef.h>

void vela_output_key_format(char *out, size_t size, const char *make,
    const char *model, const char *serial, const char *fallback);

#endif
