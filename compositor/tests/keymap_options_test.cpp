// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The XKB options a keyboard gets (keymap_options.c, docs/keyboard.md §6).

extern "C" {
#include "keymap_options.h"
}
#include <gtest/gtest.h>
#include <string>

namespace {

std::string options(const char *layout, const char *system, const char *vela)
{
    char out[128];
    EXPECT_TRUE(vela_keymap_options(layout, system, vela, out, sizeof(out)));
    return out;
}

} // namespace

TEST(KeymapOptions, VelasKeyWinsEvenEmpty)
{
    EXPECT_EQ(options("it", "terminate:ctrl_alt_bksp", "caps:escape"), "caps:escape");
    // Settings wrote no option: none, not the system's.
    EXPECT_EQ(options("it", "caps:escape", ""), "");
}

TEST(KeymapOptions, TheSystemsWithoutVelasKey)
{
    EXPECT_EQ(options("it", "caps:escape", nullptr), "caps:escape");
    EXPECT_EQ(options("it", nullptr, nullptr), "");
    EXPECT_EQ(options(nullptr, "", nullptr), "");
}

TEST(KeymapOptions, SeveralLayoutsSwitchWithWinSpace)
{
    EXPECT_EQ(options("it,us", "", nullptr), "grp:win_space_toggle");
    EXPECT_EQ(options("it,us", nullptr, "caps:escape"), "caps:escape,grp:win_space_toggle");
    // A layout switch already chosen stays the only one.
    EXPECT_EQ(options("it,us", "grp:alt_shift_toggle", nullptr), "grp:alt_shift_toggle");
    EXPECT_EQ(options("it,us", nullptr, "caps:escape, grp:ctrl_shift_toggle"), "caps:escape, grp:ctrl_shift_toggle");
    // "grp_led:" is not a switch.
    EXPECT_EQ(options("it,us", nullptr, "grp_led:caps"), "grp_led:caps,grp:win_space_toggle");
    EXPECT_EQ(options("it", nullptr, ""), "");
}

TEST(KeymapOptions, TooLongIsEmpty)
{
    char out[8] = "garbage";
    EXPECT_FALSE(vela_keymap_options("it,us", nullptr, "caps:escape", out, sizeof(out)));
    EXPECT_STREQ(out, "");
    EXPECT_FALSE(vela_keymap_options("it", nullptr, "", out, 0));
}
