// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "probe.h"

#include "device.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

void vela_print_log(FILE *out, const struct vela_transaction_log *log, const char *indent)
{
    for (int i = 0; i < log->attempts; ++i) {
        const struct vela_attempt *attempt = &log->attempt[i];
        fprintf(out, "%sattempt %d: %" PRId64 " ms, ", indent, i + 1, attempt->ms);
        if (attempt->error) {
            fprintf(out, "%s\n", strerror(attempt->error));
            continue;
        }
        for (size_t b = 0; b < attempt->length; ++b) {
            fprintf(out, "%02x ", attempt->bytes[b]);
        }
        fprintf(out, "(%s)\n", vela_ddc_reply_name(attempt->reply));
    }
}

static void probe_connector(FILE *out, const char *sysfs, const char *fake_path, const char *connector,
    int *failures)
{
    struct vela_device device;
    char error[256];
    if (vela_device_open(&device, connector, sysfs, fake_path, &vela_system_clock, error, sizeof(error)) < 0) {
        fprintf(out, "%s: %s\n", connector, error);
        ++*failures;
        return;
    }
    if (!device.has_bus) {
        fprintf(out, "%s: no bus\n", connector);
        return;
    }
    if (device.is_fake) {
        fprintf(out, "%s: fake monitor\n", connector);
    } else {
        fprintf(out, "%s: bus %s (%s)\n", connector, device.bus, device.how);
    }
    struct vela_transaction_log log;
    uint16_t value = 0;
    uint16_t max = 0;
    enum vela_result result = vela_ddc_read(&device.transport, &vela_system_clock, &value, &max, &log);
    if (log.open_error) {
        fprintf(out, "  open: %s\n", strerror(log.open_error));
    }
    vela_print_log(out, &log, "  ");
    fprintf(out, "  result: %s", vela_result_name(result));
    if (result == VELA_RESULT_OK) {
        fprintf(out, " %u/%u", value, max);
    }
    fprintf(out, " in %" PRId64 " ms (lock %" PRId64 " ms), transfers %s\n", log.total_ms, log.lock_ms,
        device.transport.ops->mode(device.transport.data));
}

static bool is_connected(const char *sysfs, const char *connector)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/class/drm/%s/status", sysfs, connector);
    FILE *file = fopen(path, "re");
    char status[32] = "";
    if (file) {
        if (!fgets(status, sizeof(status), file)) {
            status[0] = '\0';
        }
        fclose(file);
    }
    return strncmp(status, "connected", 9) == 0;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

struct names {
    char *name[64];
    int count;
};

static void add_name(void *data, const char *name)
{
    struct names *names = data;
    if (names->count < 64) {
        names->name[names->count++] = strdup(name);
    }
}

int vela_probe(FILE *out, const char *sysfs, const char *fake_path, const char *only)
{
    int failures = 0;
    if (only) {
        probe_connector(out, sysfs, fake_path, only, &failures);
        return failures ? 1 : 0;
    }
    struct names names = { .count = 0 };
    if (fake_path && *fake_path) {
        FILE *file = fopen(fake_path, "re");
        if (!file) {
            fprintf(out, "%s: %s\n", fake_path, strerror(errno));
            return 1;
        }
        char text[16384];
        size_t length = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[length] = '\0';
        vela_fake_sections(text, add_name, &names);
    } else {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/class/drm", sysfs);
        DIR *entries = opendir(dir);
        if (!entries) {
            fprintf(out, "%s: %s\n", dir, strerror(errno));
            return 1;
        }
        struct dirent *entry;
        while ((entry = readdir(entries))) {
            // card1-DP-3, not card1 nor renderD128 nor card1-Writeback-1
            if (strncmp(entry->d_name, "card", 4) == 0 && strchr(entry->d_name, '-')
                && !strstr(entry->d_name, "Writeback")) {
                add_name(&names, entry->d_name);
            }
        }
        closedir(entries);
        qsort(names.name, (size_t)names.count, sizeof(names.name[0]), compare_names);
    }
    for (int i = 0; i < names.count; ++i) {
        if (!(fake_path && *fake_path) && !is_connected(sysfs, names.name[i])) {
            fprintf(out, "%s: disconnected\n", names.name[i]);
        } else {
            probe_connector(out, sysfs, fake_path, names.name[i], &failures);
        }
        free(names.name[i]);
    }
    return failures ? 1 : 0;
}

// ------------------------------------------------------------- --bench --

static bool bench_read(struct vela_transport *transport, struct vela_clock *clock, uint16_t *value,
    uint16_t *max, struct vela_transaction_log *log)
{
    return vela_ddc_read(transport, clock, value, max, log) == VELA_RESULT_OK;
}

static void bench_write(FILE *out, struct vela_transport *transport, struct vela_clock *clock, uint16_t value)
{
    if (vela_ddc_write(transport, clock, value, NULL) != VELA_RESULT_OK) {
        fprintf(out, " (write %u failed)", value);
    }
}

// Writes `value` until a read back (after a long pause) says it's there.
static bool settle_at(FILE *out, struct vela_transport *transport, struct vela_clock *clock, uint16_t value)
{
    for (int i = 0; i < 6; ++i) {
        bench_write(out, transport, clock, value);
        clock->sleep(clock->data, 1500);
        uint16_t got, max;
        if (bench_read(transport, clock, &got, &max, NULL) && got == value) {
            return true;
        }
    }
    return false;
}

int vela_bench(FILE *out, struct vela_transport *transport, struct vela_clock *clock)
{
    uint16_t start = 0;
    uint16_t max = 0;
    struct vela_transaction_log log;
    if (!bench_read(transport, clock, &start, &max, &log)) {
        fprintf(out, "no brightness to measure\n");
        vela_print_log(out, &log, "  ");
        return 1;
    }
    fprintf(out, "start: %u/%u\n", start, max);
    // Two values below the start (or above, near 0), far enough to tell.
    uint16_t step = max >= 20 ? max / 10 : 1;
    uint16_t a = start >= 2 * step ? start - step : start + step;
    uint16_t b = start >= 2 * step ? start - 2 * step : start + 2 * step;

    fprintf(out, "\nreads (20): attempts and duration\n");
    int histogram[VELA_DDC_READ_ATTEMPTS + 1] = { 0 };
    int64_t total = 0, worst = 0;
    for (int i = 0; i < 20; ++i) {
        uint16_t value, m;
        bool ok = bench_read(transport, clock, &value, &m, &log);
        ++histogram[ok ? log.attempts : 0];
        total += log.total_ms;
        worst = log.total_ms > worst ? log.total_ms : worst;
        clock->sleep(clock->data, 200);
    }
    for (int i = 1; i <= VELA_DDC_READ_ATTEMPTS; ++i) {
        fprintf(out, "  %d attempt(s): %d\n", i, histogram[i]);
    }
    fprintf(out, "  failed: %d\n  mean %" PRId64 " ms, worst %" PRId64 " ms\n", histogram[0], total / 20, worst);

    // §5.4's interval: two writes `gap` apart, which one stays?
    fprintf(out, "\nwrite %u then %u after a gap: value read 1.5 s later\n", a, b);
    static const int gaps[] = { 0, 20, 50, 100, 150, 160, 170, 180, 190, 200, 250, 300 };
    for (size_t g = 0; g < sizeof(gaps) / sizeof(gaps[0]); ++g) {
        fprintf(out, "  %3d ms:", gaps[g]);
        for (int round = 0; round < 3; ++round) {
            if (!settle_at(out, transport, clock, start)) {
                fprintf(out, " (can't go back to %u)", start);
                continue;
            }
            bench_write(out, transport, clock, a);
            clock->sleep(clock->data, gaps[g]);
            bench_write(out, transport, clock, b);
            clock->sleep(clock->data, 1500);
            uint16_t value, m;
            if (bench_read(transport, clock, &value, &m, NULL)) {
                fprintf(out, " %u", value);
            } else {
                fprintf(out, " ✗");
            }
        }
        fprintf(out, "\n");
    }

    // §5.4's read back: how soon after a write does a read tell the truth?
    fprintf(out, "\nread back `delay` ms after writing: attempts, duration, right value\n");
    static const int delays[] = { 50, 100, 200, 300, 500, 750, 1000 };
    for (size_t d = 0; d < sizeof(delays) / sizeof(delays[0]); ++d) {
        fprintf(out, "  %4d ms:", delays[d]);
        for (int round = 0; round < 4; ++round) {
            uint16_t wanted = round % 2 ? a : b;
            if (!settle_at(out, transport, clock, round % 2 ? b : a)) {
                fprintf(out, " (unsettled)");
                continue;
            }
            bench_write(out, transport, clock, wanted);
            clock->sleep(clock->data, delays[d]);
            uint16_t value, m;
            bool ok = bench_read(transport, clock, &value, &m, &log);
            if (ok) {
                fprintf(out, " %d/%" PRId64 "ms/%s", log.attempts, log.total_ms, value == wanted ? "ok" : "old");
            } else {
                fprintf(out, " ✗/%" PRId64 "ms", log.total_ms);
            }
        }
        fprintf(out, "\n");
    }

    // Does a value that read right stay? One write, then reads at fixed
    // times after it: n (the new value), o (the old one), ✗ (no reply).
    static const int times[] = { 50, 300, 500, 750, 1000, 1500, 2000, 3000 };
    fprintf(out, "\nafter one write, the value read at");
    for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); ++t) {
        fprintf(out, " %d", times[t]);
    }
    fprintf(out, " ms\n");
    for (int round = 0; round < 8; ++round) {
        uint16_t wanted = round % 2 ? a : b;
        uint16_t old = round % 2 ? b : a;
        fprintf(out, "  %u -> %u:", old, wanted);
        if (!settle_at(out, transport, clock, old)) {
            fprintf(out, " (unsettled)\n");
            continue;
        }
        int64_t written = clock->monotonic(clock->data);
        bench_write(out, transport, clock, wanted);
        for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); ++t) {
            int64_t wait = written + times[t] - clock->monotonic(clock->data);
            if (wait > 0) {
                clock->sleep(clock->data, wait);
            }
            uint16_t value, m;
            if (!bench_read(transport, clock, &value, &m, NULL)) {
                fprintf(out, " ✗");
            } else {
                fprintf(out, " %s", value == wanted ? "n" : value == old ? "o" : "?");
            }
        }
        fprintf(out, "\n");
    }

    bool back = settle_at(out, transport, clock, start);
    fprintf(out, "\nback to %u: %s\n", start, back ? "yes" : "NO, set it with the monitor's buttons");
    return back ? 0 : 1;
}
