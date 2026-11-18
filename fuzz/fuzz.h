// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_FUZZ_H
#define VELA_FUZZ_H

// Shared by the fuzz targets (docs/testing.md, "Fuzzing"). Each target
// defines LLVMFuzzerTestOneInput; libFuzzer or replay.c calls it.

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Each target uses one fixture name. Write data to that file under a private
// XDG_CONFIG_HOME, return its path, and fail the run if setup or writing fails.
const char *vela_fuzz_config_file(const char *name, const uint8_t *data, size_t size);

#endif
