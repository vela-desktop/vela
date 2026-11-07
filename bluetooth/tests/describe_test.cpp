// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Device types and names (describe.h).

extern "C" {
#include "describe.h"
}

#include <gtest/gtest.h>

#include <string>

TEST(Describe, TypeFromIcon)
{
    EXPECT_STREQ(vela_bt_device_type("input-mouse", 0), "mouse");
    EXPECT_STREQ(vela_bt_device_type("input-keyboard", 0), "keyboard");
    EXPECT_STREQ(vela_bt_device_type("input-gaming", 0), "gamepad");
    EXPECT_STREQ(vela_bt_device_type("audio-headset", 0), "headset");
    EXPECT_STREQ(vela_bt_device_type("audio-headphones", 0), "headphones");
    EXPECT_STREQ(vela_bt_device_type("audio-card", 0), "speaker");
    EXPECT_STREQ(vela_bt_device_type("phone", 0), "phone");
    EXPECT_STREQ(vela_bt_device_type("printer", 0), "other");
    // The icon wins over the class.
    EXPECT_STREQ(vela_bt_device_type("input-mouse", 0x240404), "mouse");
}

TEST(Describe, TypeFromClass)
{
    EXPECT_STREQ(vela_bt_device_type(nullptr, 0x240404), "headset"); // wearable headset
    EXPECT_STREQ(vela_bt_device_type("", 0x240418), "headphones");
    EXPECT_STREQ(vela_bt_device_type(nullptr, 0x240414), "speaker");
    EXPECT_STREQ(vela_bt_device_type(nullptr, 0x002540), "keyboard");
    EXPECT_STREQ(vela_bt_device_type(nullptr, 0x002580), "mouse");
    EXPECT_STREQ(vela_bt_device_type(nullptr, 0x002508), "gamepad");
    EXPECT_STREQ(vela_bt_device_type(nullptr, 0x5a020c), "phone");
    EXPECT_STREQ(vela_bt_device_type(nullptr, 0x10010c), "computer");
    EXPECT_STREQ(vela_bt_device_type(nullptr, 0), "other");
}

TEST(Describe, Names)
{
    EXPECT_TRUE(vela_bt_valid_name(""));
    EXPECT_TRUE(vela_bt_valid_name("Cuffie di Brando"));
    EXPECT_TRUE(vela_bt_valid_name("Mäuschen 🎧"));
    EXPECT_TRUE(vela_bt_valid_name(std::string(248, 'a').c_str()));
    EXPECT_FALSE(vela_bt_valid_name(std::string(249, 'a').c_str()));
    EXPECT_FALSE(vela_bt_valid_name("tab\there"));
    EXPECT_FALSE(vela_bt_valid_name("new\nline"));
    EXPECT_FALSE(vela_bt_valid_name("del\x7f"));
    EXPECT_FALSE(vela_bt_valid_name("c1\xc2\x85"));
    EXPECT_FALSE(vela_bt_valid_name("\xff"));
    EXPECT_FALSE(vela_bt_valid_name("cut\xe2\x82"));
    EXPECT_FALSE(vela_bt_valid_name("overlong\xc0\xaf"));
    EXPECT_FALSE(vela_bt_valid_name("surrogate\xed\xa0\x80"));
}
