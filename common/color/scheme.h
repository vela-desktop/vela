// SPDX-FileCopyrightText: 2023 Google LLC
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: Apache-2.0
// Material's Tonal Spot scheme: the color roles of a light or dark UI from
// one source color, at the standard contrast level. Derived from Material
// Color Utilities' cpp/scheme/scheme_tonal_spot and cpp/dynamiccolor
// (commit 194144d); see common/color/LICENSE.
/*
Copyright 2023 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Changed for Vela, 2026: one variant (Tonal Spot) and one contrast level
(standard); the roles as a plain struct.
*/

#pragma once

#include "hct.h"

namespace vela::color {

// The roles of Material's color system that Vela reads (MaterialDynamicColors,
// with on_* as on*). Opaque colors.
struct Roles {
    Argb primary, onPrimary, primaryContainer, onPrimaryContainer;
    Argb secondary, onSecondary, secondaryContainer, onSecondaryContainer;
    Argb tertiary, onTertiary, tertiaryContainer, onTertiaryContainer;
    Argb error, onError, errorContainer, onErrorContainer;
    Argb surface, surfaceDim, surfaceBright, surfaceContainerLowest, surfaceContainerLow;
    Argb surfaceContainer, surfaceContainerHigh, surfaceContainerHighest;
    Argb onSurface, onSurfaceVariant, outline, outlineVariant;
    Argb inverseSurface, inverseOnSurface, inversePrimary;
};

// The Tonal Spot scheme of a source color (its alpha is ignored), light or
// dark: what Android derives from a wallpaper or a chosen color by default.
Roles tonalSpot(Argb source, bool dark);

// Each role's name, as in Roles: the keys of Style.tonal (appearance.h).
struct RoleName {
    const char* name;
    Argb Roles::* member;
};

inline constexpr RoleName roleNames[] = {
    { "primary", &Roles::primary },
    { "onPrimary", &Roles::onPrimary },
    { "primaryContainer", &Roles::primaryContainer },
    { "onPrimaryContainer", &Roles::onPrimaryContainer },
    { "secondary", &Roles::secondary },
    { "onSecondary", &Roles::onSecondary },
    { "secondaryContainer", &Roles::secondaryContainer },
    { "onSecondaryContainer", &Roles::onSecondaryContainer },
    { "tertiary", &Roles::tertiary },
    { "onTertiary", &Roles::onTertiary },
    { "tertiaryContainer", &Roles::tertiaryContainer },
    { "onTertiaryContainer", &Roles::onTertiaryContainer },
    { "error", &Roles::error },
    { "onError", &Roles::onError },
    { "errorContainer", &Roles::errorContainer },
    { "onErrorContainer", &Roles::onErrorContainer },
    { "surface", &Roles::surface },
    { "surfaceDim", &Roles::surfaceDim },
    { "surfaceBright", &Roles::surfaceBright },
    { "surfaceContainerLowest", &Roles::surfaceContainerLowest },
    { "surfaceContainerLow", &Roles::surfaceContainerLow },
    { "surfaceContainer", &Roles::surfaceContainer },
    { "surfaceContainerHigh", &Roles::surfaceContainerHigh },
    { "surfaceContainerHighest", &Roles::surfaceContainerHighest },
    { "onSurface", &Roles::onSurface },
    { "onSurfaceVariant", &Roles::onSurfaceVariant },
    { "outline", &Roles::outline },
    { "outlineVariant", &Roles::outlineVariant },
    { "inverseSurface", &Roles::inverseSurface },
    { "inverseOnSurface", &Roles::inverseOnSurface },
    { "inversePrimary", &Roles::inversePrimary },
};

// The contrast ratio of two tones (L*), 1 to 21: WCAG's, from their
// luminance.
double ratioOfTones(double toneA, double toneB);

} // namespace vela::color
