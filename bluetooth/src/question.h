// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BT_QUESTION_H
#define VELA_BT_QUESTION_H

// The agent's questions (docs/bluetooth.md §6.4, §7.2): their kinds, the
// answers each kind accepts, the legacy PIN rule and the names of
// services. No D-Bus here: plain C, unit-tested.

#include <stdbool.h>
#include <stdint.h>

enum vela_bt_question_kind {
    VELA_BT_CONFIRM_PASSKEY,
    VELA_BT_AUTHORIZE_PAIRING,
    VELA_BT_AUTHORIZE_SERVICE,
    VELA_BT_ENTER_PASSKEY,
    VELA_BT_ENTER_PIN,
    VELA_BT_SHOW_PASSKEY,
    VELA_BT_SHOW_PIN,
};

// "confirm-passkey", "authorize-pairing"...
const char *vela_bt_question_kind_name(enum vela_bt_question_kind kind);

enum vela_bt_response {
    VELA_BT_ACCEPT,
    VELA_BT_REJECT,
    VELA_BT_CANCEL,
};

enum vela_bt_answer_check {
    VELA_BT_ANSWER_OK,
    VELA_BT_WRONG_RESPONSE, // not a response this kind accepts
    VELA_BT_INVALID_VALUE,
};

// Checks an Answer to a question of `kind` (§7.2). On VELA_BT_ANSWER_OK,
// `response` is set, and `passkey` for an accepted enter-passkey.
// accept, reject: confirm-passkey, authorize-pairing, authorize-service,
//   with an empty value;
// accept: enter-passkey with a decimal 0-999999, enter-pin with 1-16 bytes
//   of UTF-8 and no control characters;
// cancel: enter-passkey, enter-pin, show-passkey, show-pin, with an empty
//   value.
enum vela_bt_answer_check vela_bt_check_answer(enum vela_bt_question_kind kind, const char *response,
    const char *value, enum vela_bt_response *out, uint32_t *passkey);

// How a legacy PIN request (RequestPinCode) is answered (§6.4).
enum vela_bt_legacy {
    VELA_BT_LEGACY_ASK, // the user types the PIN
    VELA_BT_LEGACY_0000,
    VELA_BT_LEGACY_SHOW_RANDOM, // a random 6-digit PIN, shown for typing on the device
};

// For a device of `type` (vela_bt_device_type), and whether an Add of
// the user's is behind the request: a keyboard gets a random PIN; mice,
// gamepads and audio devices get 0000; any other device, and any request
// the user did not start, asks the user.
enum vela_bt_legacy vela_bt_legacy_pin(const char *type, bool solicited);

// A service's name in plain words, from its UUID ("audio streaming" for
// A2DP); the UUID itself when it isn't known.
const char *vela_bt_service_name(const char *uuid);

#endif
