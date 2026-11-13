// SPDX-FileCopyrightText: 2022 Google LLC
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: Apache-2.0
// Derived from Material Color Utilities' cpp/palettes/tones.cc (commit
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

Changed for Vela, 2026: names and namespace of this library.
*/

#include "palette.h"

namespace vela::color {

Argb TonalPalette::tone(double tone) const
{
    return argbFromHct(m_hue, m_chroma, tone);
}

} // namespace vela::color
