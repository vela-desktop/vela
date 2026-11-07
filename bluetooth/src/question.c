// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "question.h"

#include "describe.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *vela_bt_question_kind_name(enum vela_bt_question_kind kind)
{
    static const char *const names[] = {
        [VELA_BT_CONFIRM_PASSKEY] = "confirm-passkey",
        [VELA_BT_AUTHORIZE_PAIRING] = "authorize-pairing",
        [VELA_BT_AUTHORIZE_SERVICE] = "authorize-service",
        [VELA_BT_ENTER_PASSKEY] = "enter-passkey",
        [VELA_BT_ENTER_PIN] = "enter-pin",
        [VELA_BT_SHOW_PASSKEY] = "show-passkey",
        [VELA_BT_SHOW_PIN] = "show-pin",
    };
    return (unsigned)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "";
}

static bool valid_passkey(const char *value, uint32_t *passkey)
{
    size_t length = strlen(value);
    if (length == 0 || length > 6 || strspn(value, "0123456789") != length) {
        return false;
    }
    *passkey = (uint32_t)strtoul(value, NULL, 10);
    return true;
}

enum vela_bt_answer_check vela_bt_check_answer(enum vela_bt_question_kind kind, const char *response,
    const char *value, enum vela_bt_response *out, uint32_t *passkey)
{
    enum vela_bt_response parsed;
    if (strcmp(response, "accept") == 0) {
        parsed = VELA_BT_ACCEPT;
    } else if (strcmp(response, "reject") == 0) {
        parsed = VELA_BT_REJECT;
    } else if (strcmp(response, "cancel") == 0) {
        parsed = VELA_BT_CANCEL;
    } else {
        return VELA_BT_WRONG_RESPONSE;
    }
    bool decides = kind == VELA_BT_CONFIRM_PASSKEY || kind == VELA_BT_AUTHORIZE_PAIRING
        || kind == VELA_BT_AUTHORIZE_SERVICE;
    bool enters = kind == VELA_BT_ENTER_PASSKEY || kind == VELA_BT_ENTER_PIN;
    bool shows = kind == VELA_BT_SHOW_PASSKEY || kind == VELA_BT_SHOW_PIN;
    if ((decides && parsed == VELA_BT_CANCEL) || (enters && parsed == VELA_BT_REJECT)
        || (shows && parsed != VELA_BT_CANCEL) || (!decides && !enters && !shows)) {
        return VELA_BT_WRONG_RESPONSE;
    }
    if (parsed == VELA_BT_ACCEPT && kind == VELA_BT_ENTER_PASSKEY) {
        if (!valid_passkey(value, passkey)) {
            return VELA_BT_INVALID_VALUE;
        }
    } else if (parsed == VELA_BT_ACCEPT && kind == VELA_BT_ENTER_PIN) {
        if (!*value || strlen(value) > 16 || !vela_bt_valid_name(value)) {
            return VELA_BT_INVALID_VALUE;
        }
    } else if (*value) {
        return VELA_BT_INVALID_VALUE;
    }
    *out = parsed;
    return VELA_BT_ANSWER_OK;
}

enum vela_bt_legacy vela_bt_legacy_pin(const char *type, bool solicited)
{
    if (!solicited || !type) {
        return VELA_BT_LEGACY_ASK;
    }
    if (strcmp(type, "keyboard") == 0) {
        return VELA_BT_LEGACY_SHOW_RANDOM;
    }
    static const char *const fixed[] = { "mouse", "gamepad", "headset", "headphones", "speaker" };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
        if (strcmp(type, fixed[i]) == 0) {
            return VELA_BT_LEGACY_0000;
        }
    }
    return VELA_BT_LEGACY_ASK;
}

const char *vela_bt_service_name(const char *uuid)
{
    // Bluetooth Assigned Numbers: 16-bit UUIDs on the Bluetooth base UUID.
    static const char base[] = "-0000-1000-8000-00805f9b34fb";
    static const struct {
        unsigned short number;
        const char *name;
    } services[] = {
        { 0x1101, "a serial port" },
        { 0x1105, "file sharing" },
        { 0x1106, "file transfer" },
        { 0x1108, "headset audio" },
        { 0x110a, "audio streaming" },
        { 0x110b, "audio streaming" },
        { 0x110c, "media controls" },
        { 0x110d, "audio streaming" },
        { 0x110e, "media controls" },
        { 0x110f, "media controls" },
        { 0x1112, "headset audio" },
        { 0x1115, "network sharing" },
        { 0x1116, "network sharing" },
        { 0x1117, "network sharing" },
        { 0x111e, "phone calls" },
        { 0x111f, "phone calls" },
        { 0x1124, "input (keyboard, mouse or controller)" },
        { 0x112e, "contacts" },
        { 0x112f, "contacts" },
        { 0x1130, "contacts" },
        { 0x1132, "messages" },
        { 0x1133, "messages" },
        { 0x1134, "messages" },
        { 0x1200, "device information" },
        { 0x180f, "battery level" },
        { 0x1812, "input (keyboard, mouse or controller)" },
        { 0x184e, "audio streaming" },
        { 0x1850, "audio streaming" },
    };
    if (!uuid || strlen(uuid) != 36 || strncmp(uuid, "0000", 4) != 0 || strcasecmp(uuid + 8, base) != 0
        || strspn(uuid + 4, "0123456789abcdefABCDEF") < 4) {
        return uuid ? uuid : "";
    }
    char digits[5] = { uuid[4], uuid[5], uuid[6], uuid[7], '\0' };
    unsigned long number = strtoul(digits, NULL, 16);
    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); ++i) {
        if (services[i].number == number) {
            return services[i].name;
        }
    }
    return uuid;
}
