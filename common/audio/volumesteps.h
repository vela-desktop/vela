// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The volume's steps, on the scale people see (wpctl's and pactl's, the cubic
// one, 1.0 = 100). Usually an even number: 2 points per key, wheel notch or
// slider position. A device that only has a few positions (Bluetooth headsets
// in call mode: 16, some USB devices: 31) takes them one at a time instead,
// whatever number they fall on.

#include <algorithm>
#include <cmath>

namespace VolumeSteps {

// From 2 points apart or closer, the even grid works on the device too.
inline constexpr int evenGrid = 50;

// The intervals between the device's positions, from PipeWire's volumeStep
// (1 / the number of positions, Route parameter of the device): Bluetooth
// A2DP 128 positions -> 127, HFP 16 -> 15; sound cards are continuous
// (65536). No volumeStep (0): continuous.
inline int intervals(double volumeStep)
{
    if (!(volumeStep > 0.0) || volumeStep >= 1.0) {
        return evenGrid;
    }
    return std::max(1, int(std::lround(1.0 / volumeStep)) - 1);
}

// The grid the volume moves on: 2 points, or the device's own positions when
// they are farther apart.
inline int grid(int deviceIntervals)
{
    return std::min(deviceIntervals, evenGrid);
}

// The same absolute tolerance used to confirm an asynchronous volume write.
// PipeWire's hardware readback may not be the requested grid value (e.g. a
// requested 60% can come back as 59.6%). Treat it as that grid position so
// the next notch *advances*, instead of requesting 60% forever. Use a volume
// tolerance rather than a fraction of a step: devices have different grids.
inline constexpr double readbackTolerance = 0.006;

// The next position up (direction > 0) or down from `volume`. Off the grid
// (another program set an odd number) the first step lands on it: 33 -> 34
// up, 32 down.
inline double step(double volume, int direction, int deviceIntervals)
{
    const int n = grid(deviceIntervals);
    const double position = std::clamp(volume, 0.0, 1.0) * n;
    const double nearest = std::round(position);
    // A device with more positions than the grid sets the one nearest to
    // what was asked: up to half of its own step away (60% on 64 intervals
    // reads back as 59.375%). Exactly half counts too: either neighbour may
    // be read back. The larger of the two tolerances, not their sum: a
    // position the device can hold that no request reads back as is off
    // the grid (51% on 100 intervals steps to 52%, not 54%).
    const double deviceError = deviceIntervals > n ? 0.5 * n / deviceIntervals + 1e-9 : 0.0;
    const double aligned = std::abs(position - nearest) < std::max(readbackTolerance * n, deviceError) ? nearest : position;
    const double next = direction > 0 ? std::floor(aligned) + 1 : std::ceil(aligned) - 1;
    return std::clamp(next, 0.0, double(n)) / n;
}

// The nearest position (a slider dragged by hand).
inline double snap(double volume, int deviceIntervals)
{
    const int n = grid(deviceIntervals);
    return std::clamp(std::round(volume * n), 0.0, double(n)) / n;
}

} // namespace VolumeSteps
