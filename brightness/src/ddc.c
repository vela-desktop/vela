// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ddc.h"

// The addresses that take part in the checksums: the monitor's (0x37 << 1)
// for what the host sends, and the "virtual host" address the standard uses
// for what the monitor sends.
#define MONITOR_WRITE 0x6e
#define HOST_VIRTUAL 0x50

// Host → monitor: the source address 0x51, then 0x80 | the length of what
// follows.
#define HOST_SOURCE 0x51
#define OPCODE_GET 0x01
#define OPCODE_REPLY 0x02
#define OPCODE_SET 0x03

static uint8_t checksum(uint8_t start, const uint8_t *bytes, size_t length)
{
    uint8_t sum = start;
    for (size_t i = 0; i < length; ++i) {
        sum ^= bytes[i];
    }
    return sum;
}

size_t vela_ddc_encode_get(uint8_t out[VELA_DDC_GET_LENGTH], uint8_t feature)
{
    out[0] = HOST_SOURCE;
    out[1] = 0x80 | 2;
    out[2] = OPCODE_GET;
    out[3] = feature;
    out[4] = checksum(MONITOR_WRITE, out, 4);
    return VELA_DDC_GET_LENGTH;
}

size_t vela_ddc_encode_set(uint8_t out[VELA_DDC_SET_LENGTH], uint8_t feature, uint16_t value)
{
    out[0] = HOST_SOURCE;
    out[1] = 0x80 | 4;
    out[2] = OPCODE_SET;
    out[3] = feature;
    out[4] = value >> 8;
    out[5] = value & 0xff;
    out[6] = checksum(MONITOR_WRITE, out, 6);
    return VELA_DDC_SET_LENGTH;
}

// A reply: 6e 88 02 <result> <feature> <type> <max hi> <max lo> <cur hi>
// <cur lo> <checksum>, the checksum being the XOR of 0x50 and the first ten
// bytes. A null message is 6e 80 be.
enum vela_ddc_reply vela_ddc_parse_reply(const uint8_t *bytes, size_t length, uint8_t feature,
    uint16_t *value, uint16_t *max)
{
    if (length >= 3 && bytes[0] == MONITOR_WRITE && bytes[1] == 0x80
        && bytes[2] == checksum(HOST_VIRTUAL, bytes, 2)) {
        return VELA_DDC_REPLY_NULL;
    }
    if (length < VELA_DDC_REPLY_LENGTH) {
        return VELA_DDC_REPLY_BAD;
    }
    if (bytes[0] != MONITOR_WRITE || bytes[1] != (0x80 | 8) || bytes[2] != OPCODE_REPLY
        || bytes[4] != feature || bytes[10] != checksum(HOST_VIRTUAL, bytes, 10)) {
        return VELA_DDC_REPLY_BAD;
    }
    switch (bytes[3]) {
    case 0x00:
        *max = (uint16_t)(bytes[6] << 8 | bytes[7]);
        *value = (uint16_t)(bytes[8] << 8 | bytes[9]);
        return VELA_DDC_REPLY_OK;
    case 0x01:
        return VELA_DDC_REPLY_UNSUPPORTED;
    default:
        return VELA_DDC_REPLY_BAD;
    }
}

const char *vela_ddc_reply_name(enum vela_ddc_reply reply)
{
    switch (reply) {
    case VELA_DDC_REPLY_OK:
        return "ok";
    case VELA_DDC_REPLY_UNSUPPORTED:
        return "unsupported";
    case VELA_DDC_REPLY_NULL:
        return "null message";
    case VELA_DDC_REPLY_BAD:
        break;
    }
    return "bad";
}

struct vela_ddc_request vela_ddc_parse_request(const uint8_t *bytes, size_t length)
{
    struct vela_ddc_request request = { .kind = VELA_DDC_REQUEST_INVALID };
    if (length < 2 || bytes[0] != HOST_SOURCE || (bytes[1] & 0x80) == 0) {
        return request;
    }
    size_t body = bytes[1] & 0x7f;
    if (length != body + 3 || bytes[length - 1] != checksum(MONITOR_WRITE, bytes, length - 1)) {
        return request;
    }
    if (body == 2 && bytes[2] == OPCODE_GET) {
        request.kind = VELA_DDC_REQUEST_GET;
        request.feature = bytes[3];
    } else if (body == 4 && bytes[2] == OPCODE_SET) {
        request.kind = VELA_DDC_REQUEST_SET;
        request.feature = bytes[3];
        request.value = (uint16_t)(bytes[4] << 8 | bytes[5]);
    }
    return request;
}

size_t vela_ddc_encode_reply(uint8_t out[VELA_DDC_REPLY_LENGTH], uint8_t feature, bool supported,
    uint16_t value, uint16_t max)
{
    out[0] = MONITOR_WRITE;
    out[1] = 0x80 | 8;
    out[2] = OPCODE_REPLY;
    out[3] = supported ? 0x00 : 0x01;
    out[4] = feature;
    out[5] = 0x00; // "set parameter", not momentary
    out[6] = max >> 8;
    out[7] = max & 0xff;
    out[8] = value >> 8;
    out[9] = value & 0xff;
    out[10] = checksum(HOST_VIRTUAL, out, 10);
    return VELA_DDC_REPLY_LENGTH;
}
