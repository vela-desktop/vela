// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VELA_GPU_CALIBRATION_H
#define VELA_GPU_CALIBRATION_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

struct GpuCalibration {
    uint32_t iterations;
    unsigned samples;
    double duration_ms;
};

// The caller measures the complete job in the chosen clock domain. Bound both
// the search and iteration count; never accept an unconverged final estimate.
static inline bool gpu_calibrate(double target_ms, uint32_t minimum,
                                double (*measure)(uint32_t, void*), void* context,
                                struct GpuCalibration* result)
{
    const uint32_t maximum = 1u << 30;
    if (!isfinite(target_ms) || target_ms <= 0 || !minimum || minimum > maximum)
        return false;
    uint32_t count = minimum > (1u << 14) ? minimum : (1u << 14);
    unsigned stable = 0;
    double work_ms = 0;
    for (unsigned sample = 1; sample <= 64; ++sample) {
        const double elapsed = measure(count, context);
        if (!isfinite(elapsed) || elapsed <= 0)
            return false;
        work_ms += elapsed;
        if (elapsed >= .9 * target_ms && elapsed <= 1.1 * target_ms) {
            // Keep exactly the same work for three consecutive verification
            // jobs, after enough warm-up for the initial GPU clock ramp.
            if (++stable >= 3 && work_ms >= fmin(500., 10. * target_ms)) {
                *result = (struct GpuCalibration){count, sample, elapsed};
                return true;
            }
            continue;
        }
        stable = 0;
        const double ratio = fmax(.25, fmin(4., target_ms / elapsed));
        const double scaled = (double)count * ratio;
        if (!isfinite(scaled) || scaled < minimum || scaled > maximum)
            return false;
        const uint32_t next = (uint32_t)scaled;
        if (next == count)
            return false;
        count = next;
    }
    return false;
}

#endif
