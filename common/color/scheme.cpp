// SPDX-FileCopyrightText: 2023 Google LLC
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: Apache-2.0
// Derived from Material Color Utilities' cpp/contrast/contrast.cc,
// cpp/dynamiccolor/dynamic_color.cc, material_dynamic_colors.cc,
// dynamic_scheme.cc and cpp/scheme/scheme_tonal_spot.cc (commit 194144d);
// see common/color/LICENSE.
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

Changed for Vela, 2026: the roles' definitions as a table (spec()) instead
of DynamicColor objects with closures, with only Tonal Spot's branches
(not monochrome, not fidelity or content) and each contrast curve's value at
the standard level (0); DynamicColor::GetTone without the reduced-contrast
and dual-background cases, which no role here reaches. Absolute values are
taken in floating point, as the TypeScript implementation (Math.abs) does.
*/

#include "scheme.h"

#include "palette.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace vela::color {

namespace {

// --- contrast.cc ---

// When the desired and the reached contrast ratio differ by more than this,
// lighter() and darker() fail: 0.04 keeps the result in the same tenth.
constexpr double kContrastRatioEpsilon = 0.04;
// Lighter and darker tones are pushed this much further, so that gamut
// mapping (which keeps L* only roughly) still reaches the ratio.
constexpr double kLuminanceGamutMapTolerance = 0.4;

double ratioOfYs(double y1, double y2)
{
    const double lighter = y1 > y2 ? y1 : y2;
    const double darker = (lighter == y2) ? y1 : y2;
    return (lighter + 5.0) / (darker + 5.0);
}

// The tone at least `ratio` lighter than `tone`, or -1 if there is none.
double lighter(double tone, double ratio)
{
    if (tone < 0.0 || tone > 100.0) {
        return -1.0;
    }
    const double darkY = yFromLstar(tone);
    const double lightY = ratio * (darkY + 5.0) - 5.0;
    const double realContrast = ratioOfYs(lightY, darkY);
    const double delta = std::fabs(realContrast - ratio);
    if (realContrast < ratio && delta > kContrastRatioEpsilon) {
        return -1;
    }
    const double value = lstarFromY(lightY) + kLuminanceGamutMapTolerance;
    if (value < 0 || value > 100) {
        return -1;
    }
    return value;
}

// The tone at least `ratio` darker than `tone`, or -1 if there is none.
double darker(double tone, double ratio)
{
    if (tone < 0.0 || tone > 100.0) {
        return -1.0;
    }
    const double lightY = yFromLstar(tone);
    const double darkY = ((lightY + 5.0) / ratio) - 5.0;
    const double realContrast = ratioOfYs(lightY, darkY);
    const double delta = std::fabs(realContrast - ratio);
    if (realContrast < ratio && delta > kContrastRatioEpsilon) {
        return -1;
    }
    const double value = lstarFromY(darkY) - kLuminanceGamutMapTolerance;
    if (value < 0 || value > 100) {
        return -1;
    }
    return value;
}

double lighterUnsafe(double tone, double ratio)
{
    const double safe = lighter(tone, ratio);
    return safe < 0.0 ? 100.0 : safe;
}

double darkerUnsafe(double tone, double ratio)
{
    const double safe = darker(tone, ratio);
    return safe < 0.0 ? 0.0 : safe;
}

// --- dynamic_color.cc ---

// People prefer white foregrounds on ~T60-70 (observed over time, and by
// Andrew Somers during research for APCA).
bool tonePrefersLightForeground(double tone)
{
    return std::round(tone) < 60;
}

// A foreground tone over bgTone that reaches `ratio` as closely as possible.
double foregroundTone(double bgTone, double ratio)
{
    const double lighterTone = lighterUnsafe(bgTone, ratio);
    const double darkerTone = darkerUnsafe(bgTone, ratio);
    const double lighterRatio = ratioOfTones(lighterTone, bgTone);
    const double darkerRatio = ratioOfTones(darkerTone, bgTone);
    if (tonePrefersLightForeground(bgTone)) {
        // When neither reaches a high ratio, the lighter one, unless the
        // darker is clearly better.
        const bool negligibleDifference
            = std::fabs(lighterRatio - darkerRatio) < 0.1 && lighterRatio < ratio && darkerRatio < ratio;
        return lighterRatio >= ratio || lighterRatio >= darkerRatio || negligibleDifference ? lighterTone
                                                                                             : darkerTone;
    }
    return darkerRatio >= ratio || darkerRatio >= lighterRatio ? darkerTone : lighterTone;
}

// --- material_dynamic_colors.cc, Tonal Spot only, standard contrast ---

enum class Role {
    Surface,
    SurfaceDim,
    SurfaceBright,
    SurfaceContainerLowest,
    SurfaceContainerLow,
    SurfaceContainer,
    SurfaceContainerHigh,
    SurfaceContainerHighest,
    OnSurface,
    OnSurfaceVariant,
    InverseSurface,
    InverseOnSurface,
    Outline,
    OutlineVariant,
    Primary,
    OnPrimary,
    PrimaryContainer,
    OnPrimaryContainer,
    InversePrimary,
    Secondary,
    OnSecondary,
    SecondaryContainer,
    OnSecondaryContainer,
    Tertiary,
    OnTertiary,
    TertiaryContainer,
    OnTertiaryContainer,
    Error,
    OnError,
    ErrorContainer,
    OnErrorContainer,
};

enum class PaletteOf { Primary, Secondary, Tertiary, Neutral, NeutralVariant, Error };

// ToneDeltaPair: `a` is `delta` nearer to the surfaces than `b` (every pair
// of these roles is 'nearer', not staying together).
struct Pair {
    Role a;
    Role b;
    double delta;
};

// A role as MaterialDynamicColors defines it: its palette and tone, whether
// it is a background (kept out of T50-59), what it must contrast with and
// how much (its ContrastCurve at the standard level).
struct Spec {
    PaletteOf palette;
    double tone;
    bool isBackground = false;
    std::optional<Role> background = std::nullopt;
    double contrast = 1.0;
    std::optional<Pair> pair = std::nullopt;
};

struct Scheme {
    bool dark;
    TonalPalette primary;
    TonalPalette secondary;
    TonalPalette tertiary;
    TonalPalette neutral;
    TonalPalette neutralVariant;
    TonalPalette error;

    const TonalPalette& palette(PaletteOf of) const
    {
        switch (of) {
        case PaletteOf::Primary: return primary;
        case PaletteOf::Secondary: return secondary;
        case PaletteOf::Tertiary: return tertiary;
        case PaletteOf::Neutral: return neutral;
        case PaletteOf::NeutralVariant: return neutralVariant;
        case PaletteOf::Error: break;
        }
        return error;
    }
};

// The surface text is measured against: the brightest in dark mode, the
// dimmest in light mode (highestSurface).
Role highestSurface(bool dark)
{
    return dark ? Role::SurfaceBright : Role::SurfaceDim;
}

Spec spec(Role role, bool dark)
{
    using P = PaletteOf;
    const Role highest = highestSurface(dark);
    const auto mode = [dark](double darkTone, double lightTone) { return dark ? darkTone : lightTone; };
    switch (role) {
    case Role::Surface: return { P::Neutral, mode(6, 98), true };
    case Role::SurfaceDim: return { P::Neutral, mode(6, 87), true };
    case Role::SurfaceBright: return { P::Neutral, mode(24, 98), true };
    case Role::SurfaceContainerLowest: return { P::Neutral, mode(4, 100), true };
    case Role::SurfaceContainerLow: return { P::Neutral, mode(10, 96), true };
    case Role::SurfaceContainer: return { P::Neutral, mode(12, 94), true };
    case Role::SurfaceContainerHigh: return { P::Neutral, mode(17, 92), true };
    case Role::SurfaceContainerHighest: return { P::Neutral, mode(22, 90), true };
    case Role::OnSurface: return { P::Neutral, mode(90, 10), false, highest, 7.0 };
    case Role::OnSurfaceVariant: return { P::NeutralVariant, mode(80, 30), false, highest, 4.5 };
    case Role::InverseSurface: return { P::Neutral, mode(90, 20) };
    case Role::InverseOnSurface: return { P::Neutral, mode(20, 95), false, Role::InverseSurface, 7.0 };
    case Role::Outline: return { P::NeutralVariant, mode(60, 50), false, highest, 3.0 };
    case Role::OutlineVariant: return { P::NeutralVariant, mode(30, 80), false, highest, 1.0 };
    case Role::Primary:
        return { P::Primary, mode(80, 40), true, highest, 4.5, Pair { Role::PrimaryContainer, Role::Primary, 10 } };
    case Role::OnPrimary: return { P::Primary, mode(20, 100), false, Role::Primary, 7.0 };
    case Role::PrimaryContainer:
        return { P::Primary, mode(30, 90), true, highest, 1.0, Pair { Role::PrimaryContainer, Role::Primary, 10 } };
    case Role::OnPrimaryContainer: return { P::Primary, mode(90, 30), false, Role::PrimaryContainer, 4.5 };
    case Role::InversePrimary: return { P::Primary, mode(40, 80), false, Role::InverseSurface, 4.5 };
    case Role::Secondary:
        return { P::Secondary, mode(80, 40), true, highest, 4.5,
            Pair { Role::SecondaryContainer, Role::Secondary, 10 } };
    case Role::OnSecondary: return { P::Secondary, mode(20, 100), false, Role::Secondary, 7.0 };
    case Role::SecondaryContainer:
        return { P::Secondary, mode(30, 90), true, highest, 1.0,
            Pair { Role::SecondaryContainer, Role::Secondary, 10 } };
    case Role::OnSecondaryContainer: return { P::Secondary, mode(90, 30), false, Role::SecondaryContainer, 4.5 };
    case Role::Tertiary:
        return { P::Tertiary, mode(80, 40), true, highest, 4.5, Pair { Role::TertiaryContainer, Role::Tertiary, 10 } };
    case Role::OnTertiary: return { P::Tertiary, mode(20, 100), false, Role::Tertiary, 7.0 };
    case Role::TertiaryContainer:
        return { P::Tertiary, mode(30, 90), true, highest, 1.0, Pair { Role::TertiaryContainer, Role::Tertiary, 10 } };
    case Role::OnTertiaryContainer: return { P::Tertiary, mode(90, 30), false, Role::TertiaryContainer, 4.5 };
    case Role::Error:
        return { P::Error, mode(80, 40), true, highest, 4.5, Pair { Role::ErrorContainer, Role::Error, 10 } };
    case Role::OnError: return { P::Error, mode(20, 100), false, Role::Error, 7.0 };
    case Role::ErrorContainer:
        return { P::Error, mode(30, 90), true, highest, 1.0, Pair { Role::ErrorContainer, Role::Error, 10 } };
    case Role::OnErrorContainer: return { P::Error, mode(90, 30), false, Role::ErrorContainer, 4.5 };
    }
    return { P::Neutral, 0 };
}

// DynamicColor::GetTone at the standard contrast level.
double toneOf(Role role, const Scheme& scheme)
{
    const Spec self = spec(role, scheme.dark);

    // Case 1: dual foreground, pair of colors with delta constraint.
    if (self.pair) {
        const Pair pair = *self.pair;
        // 'nearer': a is the nearer one in both modes.
        const Role nearer = pair.a;
        const Role farther = pair.b;
        const Spec nearerSpec = spec(nearer, scheme.dark);
        const Spec fartherSpec = spec(farther, scheme.dark);
        const bool amNearer = role == nearer;
        const double expansionDir = scheme.dark ? 1 : -1;
        const double bgTone = toneOf(*self.background, scheme);

        // 1st round: solve to min, each. A color good enough is not adjusted.
        double nTone = ratioOfTones(bgTone, nearerSpec.tone) >= nearerSpec.contrast
            ? nearerSpec.tone
            : foregroundTone(bgTone, nearerSpec.contrast);
        double fTone = ratioOfTones(bgTone, fartherSpec.tone) >= fartherSpec.contrast
            ? fartherSpec.tone
            : foregroundTone(bgTone, fartherSpec.contrast);

        if ((fTone - nTone) * expansionDir < pair.delta) {
            // 2nd round: expand farther to match delta.
            fTone = std::clamp(nTone + pair.delta * expansionDir, 0.0, 100.0);
            if ((fTone - nTone) * expansionDir < pair.delta) {
                // 3rd round: contract nearer to match delta.
                nTone = std::clamp(fTone - pair.delta * expansionDir, 0.0, 100.0);
            }
        }

        // Avoids the 50-59 awkward zone.
        if (50 <= nTone && nTone < 60) {
            // `nearer` in the awkward zone: moved away, with `farther`.
            if (expansionDir > 0) {
                nTone = 60;
                fTone = std::max(fTone, nTone + pair.delta * expansionDir);
            } else {
                nTone = 49;
                fTone = std::min(fTone, nTone + pair.delta * expansionDir);
            }
        } else if (50 <= fTone && fTone < 60) {
            // Not required to stay together; fixes just one.
            fTone = expansionDir > 0 ? 60 : 49;
        }
        return amNearer ? nTone : fTone;
    }

    // Case 2: no contrast pair; just solve for itself.
    double answer = self.tone;
    if (!self.background) {
        return answer; // No adjustment for colors with no background.
    }
    const double bgTone = toneOf(*self.background, scheme);
    const double desiredRatio = self.contrast;
    if (ratioOfTones(bgTone, answer) < desiredRatio) {
        // Rough improvement; what's good enough isn't "improved".
        answer = foregroundTone(bgTone, desiredRatio);
    }
    if (self.isBackground && 50 <= answer && answer < 60) {
        answer = ratioOfTones(49, bgTone) >= desiredRatio ? 49 : 60;
    }
    return answer;
}

Argb argbOf(Role role, const Scheme& scheme)
{
    return scheme.palette(spec(role, scheme.dark).palette).tone(toneOf(role, scheme));
}

} // namespace

double ratioOfTones(double toneA, double toneB)
{
    toneA = std::clamp(toneA, 0.0, 100.0);
    toneB = std::clamp(toneB, 0.0, 100.0);
    return ratioOfYs(yFromLstar(toneA), yFromLstar(toneB));
}

Roles tonalSpot(Argb source, bool dark)
{
    // SchemeTonalSpot: the source's hue at a moderate chroma for primary,
    // muted for secondary and the neutrals, 60 degrees away for tertiary;
    // DynamicScheme's red for errors.
    const Hct hct = hctFromArgb(source | 0xFF000000u);
    const Scheme scheme {
        dark,
        TonalPalette(hct.hue, 36.0),
        TonalPalette(hct.hue, 16.0),
        TonalPalette(detail::sanitizeDegrees(hct.hue + 60), 24.0),
        TonalPalette(hct.hue, 6.0),
        TonalPalette(hct.hue, 8.0),
        TonalPalette(25.0, 84.0),
    };
    Roles roles {};
    roles.primary = argbOf(Role::Primary, scheme);
    roles.onPrimary = argbOf(Role::OnPrimary, scheme);
    roles.primaryContainer = argbOf(Role::PrimaryContainer, scheme);
    roles.onPrimaryContainer = argbOf(Role::OnPrimaryContainer, scheme);
    roles.secondary = argbOf(Role::Secondary, scheme);
    roles.onSecondary = argbOf(Role::OnSecondary, scheme);
    roles.secondaryContainer = argbOf(Role::SecondaryContainer, scheme);
    roles.onSecondaryContainer = argbOf(Role::OnSecondaryContainer, scheme);
    roles.tertiary = argbOf(Role::Tertiary, scheme);
    roles.onTertiary = argbOf(Role::OnTertiary, scheme);
    roles.tertiaryContainer = argbOf(Role::TertiaryContainer, scheme);
    roles.onTertiaryContainer = argbOf(Role::OnTertiaryContainer, scheme);
    roles.error = argbOf(Role::Error, scheme);
    roles.onError = argbOf(Role::OnError, scheme);
    roles.errorContainer = argbOf(Role::ErrorContainer, scheme);
    roles.onErrorContainer = argbOf(Role::OnErrorContainer, scheme);
    roles.surface = argbOf(Role::Surface, scheme);
    roles.surfaceDim = argbOf(Role::SurfaceDim, scheme);
    roles.surfaceBright = argbOf(Role::SurfaceBright, scheme);
    roles.surfaceContainerLowest = argbOf(Role::SurfaceContainerLowest, scheme);
    roles.surfaceContainerLow = argbOf(Role::SurfaceContainerLow, scheme);
    roles.surfaceContainer = argbOf(Role::SurfaceContainer, scheme);
    roles.surfaceContainerHigh = argbOf(Role::SurfaceContainerHigh, scheme);
    roles.surfaceContainerHighest = argbOf(Role::SurfaceContainerHighest, scheme);
    roles.onSurface = argbOf(Role::OnSurface, scheme);
    roles.onSurfaceVariant = argbOf(Role::OnSurfaceVariant, scheme);
    roles.outline = argbOf(Role::Outline, scheme);
    roles.outlineVariant = argbOf(Role::OutlineVariant, scheme);
    roles.inverseSurface = argbOf(Role::InverseSurface, scheme);
    roles.inverseOnSurface = argbOf(Role::InverseOnSurface, scheme);
    roles.inversePrimary = argbOf(Role::InversePrimary, scheme);
    return roles;
}

} // namespace vela::color
