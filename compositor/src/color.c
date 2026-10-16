// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "color.h"

#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const float vela_identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

float vela_srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

// The white of a black body at `kelvin` (Tanner Helland), in sRGB 0-1.
static void blackbody(double kelvin, double out[3])
{
    double t = kelvin / 100.0;
    double r = 255.0;
    double g = 0.0;
    double b = 255.0;
    if (t <= 66.0) {
        g = 99.4708025861 * log(t) - 161.1195681661;
        b = t <= 19.0 ? 0.0 : 138.5177312231 * log(t - 10.0) - 305.0447927307;
    } else {
        r = 329.698727446 * pow(t - 60.0, -0.1332047592);
        g = 288.1221695283 * pow(t - 60.0, -0.0755148492);
    }
    out[0] = vela_clampd(r, 0.0, 255.0) / 255.0;
    out[1] = vela_clampd(g, 0.0, 255.0) / 255.0;
    out[2] = vela_clampd(b, 0.0, 255.0) / 255.0;
}

void vela_night_gains(double kelvin, float out[3])
{
    double white[3];
    double warm[3];
    blackbody(6500.0, white);
    blackbody(kelvin, warm);
    for (int i = 0; i < 3; ++i) {
        float reference = vela_srgb_to_linear((float)white[i]);
        if (reference < 1e-4f) {
            reference = 1e-4f;
        }
        out[i] = vela_clampf(vela_srgb_to_linear((float)warm[i]) / reference, 0.0f, 1.0f);
    }
}

double vela_night_kelvin(int strength)
{
    return 6500.0 - vela_clamp(strength, 0, 100) / 100.0 * (6500.0 - 1700.0);
}

int vela_night_temperature(const char *temperature, const char *strength)
{
    double kelvin = VELA_NIGHT_KELVIN_DEFAULT;
    if (temperature && *temperature) {
        kelvin = atoi(temperature);
    } else if (strength && *strength) {
        kelvin = vela_night_kelvin(atoi(strength));
    }
    int rounded = (int)lround(kelvin / 100.0) * 100;
    return vela_clamp(rounded, VELA_NIGHT_KELVIN_MIN, VELA_NIGHT_KELVIN_MAX);
}

void vela_matrix_multiply(const float a[9], const float b[9], float out[9])
{
    float result[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += a[r * 3 + k] * b[k * 3 + c];
            }
            result[r * 3 + c] = sum;
        }
    }
    memcpy(out, result, sizeof(result));
}

// Color blindness correction (daltonization): what a person with that color
// blindness loses (the difference from the simulation of Machado, Oliveira and
// Fernandes 2009, full severity) moves to the channels they see.
static void daltonize(const float simulation[9], const float shift[9], float out[9])
{
    float lost[9];
    for (int i = 0; i < 9; ++i) {
        lost[i] = vela_identity[i] - simulation[i];
    }
    float moved[9];
    vela_matrix_multiply(shift, lost, moved);
    for (int i = 0; i < 9; ++i) {
        out[i] = vela_identity[i] + moved[i];
    }
}

void vela_filter_matrix(const char *kind, float out[9])
{
    // Lost red and green go to green and blue; lost blue to red and green.
    static const float red_green_shift[9] = { 0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1 };
    static const float blue_shift[9] = { 1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0 };
    static const float deuteranopia[9] = { 0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f,
        -0.011820f, 0.042940f, 0.968881f };
    static const float protanopia[9] = { 0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f,
        -0.003882f, -0.048116f, 1.051998f };
    static const float tritanopia[9] = { 1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f,
        0.004733f, 0.691367f, 0.303900f };
    // Grayscale: the luminance on all three channels.
    static const float grayscale[9] = { 0.2126f, 0.7152f, 0.0722f, 0.2126f, 0.7152f, 0.0722f, 0.2126f, 0.7152f,
        0.0722f };

    if (strcmp(kind, "deuteranopia") == 0) {
        daltonize(deuteranopia, red_green_shift, out);
    } else if (strcmp(kind, "protanopia") == 0) {
        daltonize(protanopia, red_green_shift, out);
    } else if (strcmp(kind, "tritanopia") == 0) {
        daltonize(tritanopia, blue_shift, out);
    } else {
        memcpy(out, grayscale, sizeof(grayscale));
    }
}
