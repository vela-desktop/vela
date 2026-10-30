// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The agent's questions: the answers each kind accepts, the legacy PIN rule
// and the names of services (question.h).

extern "C" {
#include "question.h"
}

#include <gtest/gtest.h>

#include <string>

namespace {

// "ok accept", "ok reject", "ok cancel", "wrong" or "invalid".
std::string check(vela_bt_question_kind kind, const char* response, const char* value = "")
{
    vela_bt_response chosen = VELA_BT_ACCEPT;
    uint32_t passkey = 0;
    switch (vela_bt_check_answer(kind, response, value, &chosen, &passkey)) {
    case VELA_BT_WRONG_RESPONSE:
        return "wrong";
    case VELA_BT_INVALID_VALUE:
        return "invalid";
    case VELA_BT_ANSWER_OK:
        break;
    }
    return chosen == VELA_BT_ACCEPT ? "ok accept" : chosen == VELA_BT_REJECT ? "ok reject" : "ok cancel";
}

uint32_t passkeyOf(const char* value)
{
    vela_bt_response chosen;
    uint32_t passkey = 12345678;
    EXPECT_EQ(vela_bt_check_answer(VELA_BT_ENTER_PASSKEY, "accept", value, &chosen, &passkey), VELA_BT_ANSWER_OK);
    return passkey;
}

} // namespace

TEST(Question, KindNames)
{
    EXPECT_STREQ(vela_bt_question_kind_name(VELA_BT_CONFIRM_PASSKEY), "confirm-passkey");
    EXPECT_STREQ(vela_bt_question_kind_name(VELA_BT_AUTHORIZE_PAIRING), "authorize-pairing");
    EXPECT_STREQ(vela_bt_question_kind_name(VELA_BT_AUTHORIZE_SERVICE), "authorize-service");
    EXPECT_STREQ(vela_bt_question_kind_name(VELA_BT_ENTER_PASSKEY), "enter-passkey");
    EXPECT_STREQ(vela_bt_question_kind_name(VELA_BT_ENTER_PIN), "enter-pin");
    EXPECT_STREQ(vela_bt_question_kind_name(VELA_BT_SHOW_PASSKEY), "show-passkey");
    EXPECT_STREQ(vela_bt_question_kind_name(VELA_BT_SHOW_PIN), "show-pin");
}

TEST(Question, ConfirmAndAuthorizeTakeAcceptOrReject)
{
    for (vela_bt_question_kind kind :
        { VELA_BT_CONFIRM_PASSKEY, VELA_BT_AUTHORIZE_PAIRING, VELA_BT_AUTHORIZE_SERVICE }) {
        EXPECT_EQ(check(kind, "accept"), "ok accept");
        EXPECT_EQ(check(kind, "reject"), "ok reject");
        EXPECT_EQ(check(kind, "cancel"), "wrong");
        EXPECT_EQ(check(kind, "yes"), "wrong");
        EXPECT_EQ(check(kind, ""), "wrong");
        EXPECT_EQ(check(kind, "accept", "123456"), "invalid");
    }
}

TEST(Question, Passkey)
{
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "accept", "123456"), "ok accept");
    EXPECT_EQ(passkeyOf("123456"), 123456u);
    EXPECT_EQ(passkeyOf("0"), 0u);
    EXPECT_EQ(passkeyOf("000042"), 42u);
    EXPECT_EQ(passkeyOf("999999"), 999999u);
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "accept", ""), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "accept", "1000000"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "accept", "-1"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "accept", "+1"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "accept", " 123"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "accept", "12a456"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "accept", "0x10"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "cancel"), "ok cancel");
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "cancel", "1"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PASSKEY, "reject"), "wrong");
}

TEST(Question, Pin)
{
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "accept", "0000"), "ok accept");
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "accept", "abcdefghijklmnop"), "ok accept");
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "accept", "città"), "ok accept");
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "accept", ""), "invalid");
    // 17 bytes, also when 15 letters and an "à" make them; 14 letters and
    // an "à" are 16.
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "accept", "abcdefghijklmnopq"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "accept", "abcdefghijklmnoà"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "accept", "abcdefghijklmnà"), "ok accept");
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "accept", "12\n4"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "accept", "12\x7f"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "accept", "\xff\xfe"), "invalid");
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "cancel"), "ok cancel");
    EXPECT_EQ(check(VELA_BT_ENTER_PIN, "reject"), "wrong");
}

TEST(Question, ShownCodesOnlyCancel)
{
    for (vela_bt_question_kind kind : { VELA_BT_SHOW_PASSKEY, VELA_BT_SHOW_PIN }) {
        EXPECT_EQ(check(kind, "cancel"), "ok cancel");
        EXPECT_EQ(check(kind, "accept"), "wrong");
        EXPECT_EQ(check(kind, "reject"), "wrong");
        EXPECT_EQ(check(kind, "cancel", "x"), "invalid");
    }
}

TEST(Question, LegacyPin)
{
    EXPECT_EQ(vela_bt_legacy_pin("keyboard", true), VELA_BT_LEGACY_SHOW_RANDOM);
    for (const char* type : { "mouse", "gamepad", "headset", "headphones", "speaker" }) {
        EXPECT_EQ(vela_bt_legacy_pin(type, true), VELA_BT_LEGACY_0000) << type;
    }
    for (const char* type : { "phone", "computer", "tablet", "other" }) {
        EXPECT_EQ(vela_bt_legacy_pin(type, true), VELA_BT_LEGACY_ASK) << type;
    }
    // A request the user did not start: always typed by the user.
    for (const char* type : { "keyboard", "mouse", "headset", "phone" }) {
        EXPECT_EQ(vela_bt_legacy_pin(type, false), VELA_BT_LEGACY_ASK) << type;
    }
    EXPECT_EQ(vela_bt_legacy_pin(nullptr, true), VELA_BT_LEGACY_ASK);
}

TEST(Question, ServiceNames)
{
    EXPECT_STREQ(vela_bt_service_name("0000110b-0000-1000-8000-00805f9b34fb"), "audio streaming");
    EXPECT_STREQ(vela_bt_service_name("0000111E-0000-1000-8000-00805F9B34FB"), "phone calls");
    EXPECT_STREQ(vela_bt_service_name("00001124-0000-1000-8000-00805f9b34fb"), "input (keyboard, mouse or controller)");
    EXPECT_STREQ(vela_bt_service_name("00001812-0000-1000-8000-00805f9b34fb"), "input (keyboard, mouse or controller)");
    // Unknown, or not on the base UUID: the UUID itself.
    EXPECT_STREQ(vela_bt_service_name("0000abcd-0000-1000-8000-00805f9b34fb"), "0000abcd-0000-1000-8000-00805f9b34fb");
    EXPECT_STREQ(vela_bt_service_name("6e400001-b5a3-f393-e0a9-e50e24dcca9e"), "6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    EXPECT_STREQ(vela_bt_service_name("0000110b-0000-1000-8000-00805f9b34fc"), "0000110b-0000-1000-8000-00805f9b34fc");
    EXPECT_STREQ(vela_bt_service_name("0000zz0b-0000-1000-8000-00805f9b34fb"), "0000zz0b-0000-1000-8000-00805f9b34fb");
    EXPECT_STREQ(vela_bt_service_name("110b"), "110b");
    EXPECT_STREQ(vela_bt_service_name(""), "");
}
