// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" {
#include "super_numbers.h"
}
#include <gtest/gtest.h>
#include <linux/input-event-codes.h>

namespace {

vela_super_modifiers held(bool logo, bool ctrl = false, bool alt = false, bool shift = false)
{
    return { logo, ctrl, alt, shift };
}

} // namespace

TEST(SuperNumbers, ParsesTheSetting)
{
    EXPECT_EQ(vela_super_numbers_parse("apps"), VELA_SUPER_NUMBERS_APPS);
    EXPECT_EQ(vela_super_numbers_parse("desktops"), VELA_SUPER_NUMBERS_DESKTOPS);
    EXPECT_EQ(vela_super_numbers_parse("off"), VELA_SUPER_NUMBERS_OFF);
    // Unknown, empty or missing: the default.
    EXPECT_EQ(vela_super_numbers_parse("Desktops"), VELA_SUPER_NUMBERS_APPS);
    EXPECT_EQ(vela_super_numbers_parse(""), VELA_SUPER_NUMBERS_APPS);
    EXPECT_EQ(vela_super_numbers_parse(nullptr), VELA_SUPER_NUMBERS_APPS);
}

TEST(SuperNumbers, IndexesTheNumberRow)
{
    EXPECT_EQ(vela_super_number_index(KEY_1), 0);
    EXPECT_EQ(vela_super_number_index(KEY_5), 4);
    EXPECT_EQ(vela_super_number_index(KEY_9), 8);
    EXPECT_EQ(vela_super_number_index(KEY_0), 9);
    // Neighbours and the keypad are not the number row.
    EXPECT_EQ(vela_super_number_index(KEY_ESC), -1);
    EXPECT_EQ(vela_super_number_index(KEY_MINUS), -1);
    EXPECT_EQ(vela_super_number_index(KEY_KP1), -1);
    EXPECT_EQ(vela_super_number_index(0), -1);
}

TEST(SuperNumbers, AppsModeActions)
{
    const auto apps = VELA_SUPER_NUMBERS_APPS;
    EXPECT_EQ(vela_super_number_action(apps, held(true)), VELA_SUPER_NUMBER_ACTIVATE);
    EXPECT_EQ(vela_super_number_action(apps, held(true, false, false, true)), VELA_SUPER_NUMBER_NEW);
    EXPECT_EQ(vela_super_number_action(apps, held(true, false, true)), VELA_SUPER_NUMBER_JUMP);
    // Left to the apps.
    EXPECT_EQ(vela_super_number_action(apps, held(false)), VELA_SUPER_NUMBER_NONE);
    EXPECT_EQ(vela_super_number_action(apps, held(false, false, false, true)), VELA_SUPER_NUMBER_NONE);
    EXPECT_EQ(vela_super_number_action(apps, held(true, true)), VELA_SUPER_NUMBER_NONE);
    EXPECT_EQ(vela_super_number_action(apps, held(true, false, true, true)), VELA_SUPER_NUMBER_NONE);
}

TEST(SuperNumbers, DesktopsModeActions)
{
    const auto desktops = VELA_SUPER_NUMBERS_DESKTOPS;
    EXPECT_EQ(vela_super_number_action(desktops, held(true)), VELA_SUPER_NUMBER_SWITCH);
    EXPECT_EQ(vela_super_number_action(desktops, held(true, false, false, true)), VELA_SUPER_NUMBER_MOVE);
    EXPECT_EQ(vela_super_number_action(desktops, held(true, false, true)), VELA_SUPER_NUMBER_NONE);
    EXPECT_EQ(vela_super_number_action(desktops, held(true, true)), VELA_SUPER_NUMBER_NONE);
    EXPECT_EQ(vela_super_number_action(desktops, held(false)), VELA_SUPER_NUMBER_NONE);
}

TEST(SuperNumbers, OffLeavesEveryKeyToTheApps)
{
    for (int bits = 0; bits < 16; ++bits) {
        const vela_super_modifiers mods = held(bits & 1, bits & 2, bits & 4, bits & 8);
        EXPECT_EQ(vela_super_number_action(VELA_SUPER_NUMBERS_OFF, mods), VELA_SUPER_NUMBER_NONE) << bits;
    }
}

TEST(SuperNumbers, VerbsAreTheShellsWords)
{
    EXPECT_STREQ(vela_super_number_verb(VELA_SUPER_NUMBER_ACTIVATE), "activate");
    EXPECT_STREQ(vela_super_number_verb(VELA_SUPER_NUMBER_NEW), "new");
    EXPECT_STREQ(vela_super_number_verb(VELA_SUPER_NUMBER_JUMP), "jump");
    EXPECT_EQ(vela_super_number_verb(VELA_SUPER_NUMBER_NONE), nullptr);
    EXPECT_EQ(vela_super_number_verb(VELA_SUPER_NUMBER_SWITCH), nullptr);
    EXPECT_EQ(vela_super_number_verb(VELA_SUPER_NUMBER_MOVE), nullptr);
}

TEST(SuperNumbers, FilterDropsOnlyTheHandledNumbers)
{
    const uint32_t keys[] = { KEY_LEFTMETA, KEY_1, KEY_A, KEY_0, KEY_3 };
    uint32_t out[5] = {};
    // Super+1 and Super+0 were the compositor's; 3 was pressed for the app.
    const uint16_t handled = (1u << 0) | (1u << 9);
    ASSERT_EQ(vela_super_numbers_filter(keys, 5, handled, out), 3u);
    EXPECT_EQ(out[0], uint32_t(KEY_LEFTMETA));
    EXPECT_EQ(out[1], uint32_t(KEY_A));
    EXPECT_EQ(out[2], uint32_t(KEY_3));

    EXPECT_EQ(vela_super_numbers_filter(keys, 5, 0, out), 5u);
    EXPECT_EQ(out[1], uint32_t(KEY_1));
    EXPECT_EQ(vela_super_numbers_filter(keys, 0, 0xffff, out), 0u);
}
