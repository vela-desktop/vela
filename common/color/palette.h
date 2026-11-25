// SPDX-FileCopyrightText: 2022 Google LLC
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: Apache-2.0
// A tonal palette: one hue and chroma, any tone. Derived from Material Color
// Utilities' cpp/palettes/tones (commit 194144d); see common/color/LICENSE.
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

Changed for Vela, 2026: without the key color, which no role of the Tonal
Spot scheme reads.
*/

#pragma once

#include "hct.h"

namespace vela::color {

class TonalPalette {
public:
    TonalPalette(double hue, double chroma)
        : m_hue(hue)
        , m_chroma(chroma)
    {
    }

    // The palette's color at a tone (0 to 100); its chroma is lower where
    // sRGB has none as high at that hue and tone.
    Argb tone(double tone) const;

    double hue() const { return m_hue; }
    double chroma() const { return m_chroma; }

private:
    double m_hue;
    double m_chroma;
};

} // namespace vela::color
