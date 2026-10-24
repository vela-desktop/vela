// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_DDC_H
#define VELA_BRIGHTNESS_DDC_H

// The two DDC/CI messages brightness needs, from the DDC/CI and MCCS
// standards (docs/brightness.md §5.2): Get VCP and Set VCP of one feature,
// and the reply to Get VCP. Pure: bytes in, bytes out, no I/O.
//
// The bytes are those after the address on the wire: the monitor is at
// I²C address 0x37, written as 0x6e, which the request's checksum includes.
// The fake monitors (fake.h) use the other side: decoding a request and
// encoding a reply.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VELA_DDC_ADDRESS 0x37
#define VELA_DDC_BRIGHTNESS 0x10 // VCP feature "luminance"

#define VELA_DDC_GET_LENGTH 5
#define VELA_DDC_SET_LENGTH 7
#define VELA_DDC_REPLY_LENGTH 11

enum vela_ddc_reply {
    VELA_DDC_REPLY_OK,
    VELA_DDC_REPLY_UNSUPPORTED, // a real reply: the monitor doesn't have the feature
    VELA_DDC_REPLY_NULL, // a null message: "not ready"
    VELA_DDC_REPLY_BAD, // not a reply: wrong bytes, wrong checksum, too short
};

size_t vela_ddc_encode_get(uint8_t out[VELA_DDC_GET_LENGTH], uint8_t feature);
size_t vela_ddc_encode_set(uint8_t out[VELA_DDC_SET_LENGTH], uint8_t feature, uint16_t value);

// The reply to a Get VCP of `feature`. `value` and `max` are written only
// for VELA_DDC_REPLY_OK.
enum vela_ddc_reply vela_ddc_parse_reply(const uint8_t *bytes, size_t length, uint8_t feature,
    uint16_t *value, uint16_t *max);

const char *vela_ddc_reply_name(enum vela_ddc_reply reply);

// The monitor's side, for the fake monitors.
enum vela_ddc_request_kind {
    VELA_DDC_REQUEST_INVALID,
    VELA_DDC_REQUEST_GET,
    VELA_DDC_REQUEST_SET,
};

struct vela_ddc_request {
    enum vela_ddc_request_kind kind;
    uint8_t feature;
    uint16_t value; // for SET
};

struct vela_ddc_request vela_ddc_parse_request(const uint8_t *bytes, size_t length);
// supported = false: the reply says the feature doesn't exist.
size_t vela_ddc_encode_reply(uint8_t out[VELA_DDC_REPLY_LENGTH], uint8_t feature, bool supported,
    uint16_t value, uint16_t max);

#endif
