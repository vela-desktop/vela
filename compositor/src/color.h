// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_COLOR_H
#define VELA_COLOR_H

// The screen's color (a11y.c, docs/renderer.md §7.6): night light (the white
// of a black body and how much of each channel is left) and the color filter
// matrices, 3x3 by rows in linear space. Pure functions, tested in
// compositor/tests/color_sun_test.cpp.

float vela_srgb_to_linear(float c);


// How much red, green and blue (in linear light) is left at that temperature,
// relative to the screen's normal white (6500 K).
void vela_night_gains(double kelvin, float out[3]);

// The older setting, Windows' strength (0-100), in kelvin: from 6500 K
// (off) down to 1700 K.
double vela_night_kelvin(int strength);
// Night light's color temperature from vela.conf: night-light-temperature in
// kelvin, else the older night-light-strength (NULL or empty: absent; both
// absent: 4200 K), clamped to VELA_NIGHT_KELVIN_MIN..MAX and rounded to
// 100 K.
#define VELA_NIGHT_KELVIN_MIN 1700
#define VELA_NIGHT_KELVIN_MAX 6500
#define VELA_NIGHT_KELVIN_DEFAULT 4200
int vela_night_temperature(const char *temperature, const char *strength);

extern const float vela_identity[9];

void vela_matrix_multiply(const float a[9], const float b[9], float out[9]);

// The color filter by name: grayscale (also for unknown names), deuteranopia,
// protanopia, tritanopia.
void vela_filter_matrix(const char *kind, float out[9]);

#endif
