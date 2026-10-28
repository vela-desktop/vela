// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "describe.h"

#include <stddef.h>
#include <string.h>

const char *vela_bt_device_type(const char *icon, uint32_t class_of_device)
{
    static const struct {
        const char *icon;
        const char *type;
    } icons[] = {
        { "input-mouse", "mouse" },
        { "input-keyboard", "keyboard" },
        { "input-gaming", "gamepad" },
        { "input-tablet", "tablet" },
        { "audio-headset", "headset" },
        { "audio-headphones", "headphones" },
        { "audio-card", "speaker" },
        { "phone", "phone" },
        { "computer", "computer" },
    };
    for (size_t i = 0; icon && i < sizeof(icons) / sizeof(icons[0]); ++i) {
        if (strcmp(icon, icons[i].icon) == 0) {
            return icons[i].type;
        }
    }
    if (icon && *icon) {
        return "other";
    }
    // Bluetooth Assigned Numbers, Class of Device: major class in bits 8-12.
    uint32_t major = (class_of_device >> 8) & 0x1f;
    uint32_t minor = (class_of_device >> 2) & 0x3f;
    switch (major) {
    case 0x01:
        return "computer";
    case 0x02:
        return "phone";
    case 0x04:
        return minor == 0x01 || minor == 0x02 ? "headset" : minor == 0x06 ? "headphones" : minor == 0x05 ? "speaker" : "other";
    case 0x05:
        if (minor & 0x10) {
            return "keyboard";
        }
        if (minor & 0x20) {
            return "mouse";
        }
        return (minor & 0x0f) == 0x01 || (minor & 0x0f) == 0x02 ? "gamepad" : (minor & 0x0f) == 0x05 ? "tablet" : "other";
    default:
        return "other";
    }
}

bool vela_bt_valid_name(const char *name)
{
    const unsigned char *s = (const unsigned char *)name;
    size_t length = strlen(name);
    if (length > 248) {
        return false;
    }
    for (size_t i = 0; i < length;) {
        unsigned char c = s[i];
        uint32_t point;
        size_t extra;
        if (c < 0x80) {
            point = c;
            extra = 0;
        } else if ((c & 0xe0) == 0xc0) {
            point = c & 0x1f;
            extra = 1;
        } else if ((c & 0xf0) == 0xe0) {
            point = c & 0x0f;
            extra = 2;
        } else if ((c & 0xf8) == 0xf0) {
            point = c & 0x07;
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= length) {
            return false;
        }
        for (size_t k = 1; k <= extra; ++k) {
            if ((s[i + k] & 0xc0) != 0x80) {
                return false;
            }
            point = (point << 6) | (s[i + k] & 0x3f);
        }
        // Overlong forms, surrogates, past U+10FFFF, and control characters
        // (C0, DEL, C1).
        static const uint32_t smallest[] = { 0, 0x80, 0x800, 0x10000 };
        if (point < smallest[extra] || (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff || point < 0x20
            || (point >= 0x7f && point <= 0x9f)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}
