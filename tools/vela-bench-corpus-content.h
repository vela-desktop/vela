// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#define CORPUS_METHOD "toolkit-scaling-corpus-v1"
#define CORPUS_CONTENT "Vela toolkit corpus v1|DejaVu Sans|16px|480x320|Alpha,100%,Ready|Beta,125%,Local|Gamma,150%,Fixed|Delta,200%,Offline|0123456789 AaBb CcDd|markers:8,16"
#define CORPUS_FONT "DejaVu Sans"
#define CORPUS_WIDTH 480
#define CORPUS_HEIGHT 320
#define CORPUS_INSET 8
#define CORPUS_MARKER 16

static inline void corpus_color(int family, int corner, unsigned char rgb[3])
{
    static const unsigned char base[4][3] = {{208,32,208},{32,208,32},{32,64,224},{224,128,32}};
    rgb[0] = base[corner][0] + (corner == 0 || corner == 3 ? -16 : 16) * family;
    rgb[1] = base[corner][1] + (corner == 1 ? -16 : 16) * family;
    rgb[2] = base[corner][2] + (corner == 0 ? 0 : corner == 2 ? -16 : 16) * family;
}
