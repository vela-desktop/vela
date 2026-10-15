// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_NIGHT_LIGHT_POLICY_H
#define VELA_NIGHT_LIGHT_POLICY_H

#include <stdbool.h>

/**
 * Return whether a color change must repaint the rendered buffer.
 * The final shader fade tick still changes every pixel, whereas a gamma-only
 * commit can reuse an unmodified buffer.
 */
static inline bool vela_night_light_repaint_required(bool filter_changed, bool night_changed, bool drm,
    bool force_shader, bool gamma_refused, bool night_in_gamma)
{
    return filter_changed
        || (night_changed && (!drm || force_shader || (gamma_refused && !night_in_gamma)));
}

/**
 * Defer clearing a rejected hardware LUT unless a retry was requested.
 * Keeping the LUT prevents a simultaneous shader tint from doubling colors.
 */
static inline bool vela_night_light_defer_clear(bool gamma_refused, bool clear_needed, bool retry_pending,
    bool gamma_committed)
{
    return gamma_refused && clear_needed && !retry_pending && gamma_committed;
}

#endif
