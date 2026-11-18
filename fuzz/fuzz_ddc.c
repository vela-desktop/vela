// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// DDC/CI bytes from a monitor (brightness/src/ddc.c): the reply to Get VCP
// comes from hardware over I2C and must never be trusted. The first input
// byte is the feature asked for; the rest is what came back.

#include "ddc.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) {
        return 0;
    }
    const uint8_t feature = data[0];
    uint16_t value = 0;
    uint16_t max = 0;
    enum vela_ddc_reply reply = vela_ddc_parse_reply(data + 1, size - 1, feature, &value, &max);
    if (reply != VELA_DDC_REPLY_OK && reply != VELA_DDC_REPLY_UNSUPPORTED && reply != VELA_DDC_REPLY_NULL
        && reply != VELA_DDC_REPLY_BAD) {
        __builtin_trap();
    }
    if (reply == VELA_DDC_REPLY_OK) {
        // What was understood encodes to a reply that parses the same.
        uint8_t again[VELA_DDC_REPLY_LENGTH];
        size_t length = vela_ddc_encode_reply(again, feature, true, value, max);
        uint16_t value2 = 0;
        uint16_t max2 = 0;
        if (vela_ddc_parse_reply(again, length, feature, &value2, &max2) != VELA_DDC_REPLY_OK || value2 != value
            || max2 != max) {
            __builtin_trap();
        }
    }
    // The monitor's side, used by the fake monitors in the tests.
    struct vela_ddc_request request = vela_ddc_parse_request(data, size);
    if (request.kind != VELA_DDC_REQUEST_INVALID && request.kind != VELA_DDC_REQUEST_GET
        && request.kind != VELA_DDC_REQUEST_SET) {
        __builtin_trap();
    }
    return 0;
}
