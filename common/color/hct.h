// SPDX-FileCopyrightText: 2022 Google LLC
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: Apache-2.0
// HCT (hue, chroma, tone): CAM16's hue and chroma with L* as the tone, the
// color space Material's palettes are built in. Derived from Material Color
// Utilities' cpp/cam and cpp/utils (commit 194144d); see common/color/LICENSE.
/*
Copyright 2022 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Changed for Vela, 2026: only what the tonal scheme and the wallpaper's
source color need, in one header and namespace vela::color.
*/

#pragma once

#include <cstdint>

namespace vela::color {

// 0xAARRGGBB, as Qt's QRgb.
using Argb = std::uint32_t;

struct Hct {
    double hue = 0.0; // CAM16 hue, 0 to 360
    double chroma = 0.0; // CAM16 chroma, 0 to about 150 in sRGB
    double tone = 0.0; // L*, 0 (black) to 100 (white)
};

// The color's HCT, in the default viewing conditions (sRGB's white point,
// a gray world at L* 50: CAM16's own defaults, as Material's).
Hct hctFromArgb(Argb argb);

// The sRGB color nearest to an HCT: hue and tone kept, chroma lowered until
// it fits in sRGB (HctSolver). Always opaque.
Argb argbFromHct(double hue, double chroma, double tone);
inline Argb argbFromHct(const Hct& hct)
{
    return argbFromHct(hct.hue, hct.chroma, hct.tone);
}

// L* and the relative luminance Y (0 to 100) of WCAG's contrast ratio.
double lstarFromArgb(Argb argb);
double yFromLstar(double lstar);
double lstarFromY(double y);

namespace detail {

// sRGB channel (0-255) to linear (0-100), and back.
double linearized(int component);
int delinearized(double component);
double sanitizeDegrees(double degrees);
int sanitizeDegrees(int degrees);
// The shortest distance between two hues, 0 to 180.
double differenceDegrees(double a, double b);

} // namespace detail

} // namespace vela::color
