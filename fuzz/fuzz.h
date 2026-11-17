// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_FUZZ_H
#define VELA_FUZZ_H

// Shared by the fuzz targets (docs/testing.md, "Fuzzing"). Each target
// defines LLVMFuzzerTestOneInput; libFuzzer or replay.c calls it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Writes `data` to <private XDG_CONFIG_HOME>/vela/<name>, creating that
// directory once per process, and gives the file's path. false if it can't.
bool vela_fuzz_config_file(const char *name, const uint8_t *data, size_t size, char *path, size_t path_size);

#endif
