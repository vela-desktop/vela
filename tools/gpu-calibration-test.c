// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include <assert.h>
#include "gpu-calibration.h"

struct Clock {
    unsigned calls;
    uint32_t previous;
    unsigned unchanged;
    double work_ms;
    int mode;
};

static double measure(uint32_t iterations, void *context)
{
    struct Clock *clock = context;
    ++clock->calls;
    clock->unchanged = iterations == clock->previous ? clock->unchanged + 1 : 1;
    clock->previous = iterations;
    // Simulate a GPU doubling its execution rate as its clocks ramp up.
    double elapsed = iterations * (clock->calls <= 5 ? .00002 : .00001);
    if (clock->mode == 1) elapsed = NAN;
    if (clock->mode == 2) elapsed = 0;
    if (clock->mode == 3) elapsed = INFINITY;
    if (clock->mode == 4) elapsed = -1;
    if (clock->mode == 5) elapsed = clock->calls % 2 ? 50 : 300;
    if (clock->mode == 6) elapsed = 1e-20; // cannot reach target in range
    if (clock->mode == 7) elapsed = 1e20; // even minimum work is too long
    clock->work_ms += elapsed;
    return elapsed;
}

int main(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        const double target = (double[]) { 1, 5, 150, 1000 }[i];
        struct Clock clock = { 0 };
        struct GpuCalibration result;
        assert(gpu_calibrate(target, 50, measure, &clock, &result));
        assert(clock.calls > 5 && clock.calls <= 64);
        assert(clock.unchanged >= 3);
        assert(clock.work_ms >= fmin(500., 10. * target));
        assert(result.duration_ms >= .9 * target && result.duration_ms <= 1.1 * target);
        assert(result.iterations == clock.previous && result.samples == clock.calls);
    }
    for (int mode = 1; mode <= 7; ++mode) {
        struct Clock clock = { .mode = mode };
        struct GpuCalibration result;
        assert(!gpu_calibrate(150, 50, measure, &clock, &result));
        assert(clock.calls <= 64);
    }
    struct Clock clock = { 0 };
    struct GpuCalibration result;
    assert(!gpu_calibrate(NAN, 50, measure, &clock, &result));
    assert(!gpu_calibrate(0, 50, measure, &clock, &result));
    assert(!gpu_calibrate(150, 0, measure, &clock, &result));
    assert(clock.calls == 0);
    return 0;
}
