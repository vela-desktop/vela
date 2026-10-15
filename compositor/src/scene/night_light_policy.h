// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_NIGHT_LIGHT_POLICY_H
#define VELA_NIGHT_LIGHT_POLICY_H

#include <stdbool.h>

// A shader-tinted DRM frame needs a full repaint even on the final transition
// tick, when night_active becomes false. Gamma-only updates don't change pixels.
static inline bool vela_night_light_repaint_required(bool filter_changed, bool night_changed, bool drm,
    bool force_shader, bool gamma_refused, bool night_in_gamma)
{
    return filter_changed
        || (night_changed && (!drm || force_shader || (gamma_refused && !night_in_gamma)));
}

// After a rejected clear, keep the existing hardware LUT rather than double
// tinting. An explicit retry must override that deferral for one attempt.
static inline bool vela_night_light_defer_clear(bool gamma_refused, bool clear_needed, bool retry_pending,
    bool gamma_committed)
{
    return gamma_refused && clear_needed && !retry_pending && gamma_committed;
}

#endif
