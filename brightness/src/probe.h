// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_PROBE_H
#define VELA_BRIGHTNESS_PROBE_H

// vela-brightness --probe and --bench (docs/brightness.md §11, §5.4).
//
// --probe [connector]: each connector (or one), its bus, how the bytes
// travel, and one read with every attempt, its bytes and its latency. It
// only reads: safe to run anywhere, and what goes into a bug report.
//
// --bench <connector>: the measurements behind the timings of tuning.h,
// on a real monitor. It writes, and puts the starting value back at the end.

#include "transfer.h"

#include <stdio.h>

// 0, or 1 when a connector couldn't be probed at all.
int vela_probe(FILE *out, const char *sysfs, const char *fake_path, const char *only);

int vela_bench(FILE *out, struct vela_transport *transport, struct vela_clock *clock);

// One read's attempts, as --probe prints them.
void vela_print_log(FILE *out, const struct vela_transaction_log *log, const char *indent);

#endif
