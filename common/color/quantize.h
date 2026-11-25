// SPDX-FileCopyrightText: 2022 Google LLC
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: Apache-2.0
// An image's colors (Wu's quantizer refined by weighted k-means, Celebi's
// method) and the one most suitable as a UI's source color (Score). Derived
// from Material Color Utilities' cpp/quantize and cpp/score (commit
// 194144d); see common/color/LICENSE.
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

Changed for Vela, 2026: pixels as a span; only the colors' populations
returned; sourceFromPixels() added, as the TypeScript implementation's
sourceColorFromImageBytes().
*/

#pragma once

#include "hct.h"

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace vela::color {

// Up to maxColors (at most 256) colors of the opaque pixels, each with how
// many pixels it stands for. Pixels with any transparency are left out.
std::map<Argb, std::uint32_t> quantizeCelebi(std::span<const Argb> pixels, int maxColors);

// The colors most suitable for a UI theme, the best first: chroma near 48
// and a hue used widely, at most `desired` of them with hues far apart.
// Colors too gray (chroma below 5) or too rare (1% of their hue's
// neighborhood) are left out; if none is left, only `fallback`.
std::vector<Argb> rankedSuggestions(const std::map<Argb, std::uint32_t>& population, Argb fallback,
    std::size_t desired = 4);

// An image's source color (its pixels as 0xAARRGGBB): quantized to 128
// colors, the best scored; `fallback` for an image without any suitable
// color (grayscale, empty, transparent).
Argb sourceFromPixels(std::span<const Argb> pixels, Argb fallback);

} // namespace vela::color
