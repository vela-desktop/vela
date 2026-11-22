// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Caps Lock and Num Lock shared between keyboards, and Num Lock at startup
// (lock_keys.c, docs/keyboard.md §7, §8.1).

extern "C" {
#include "lock_keys.h"
}
#include "privatetemp.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <sys/stat.h>
#include <xkbcommon/xkbcommon.h>

namespace {

struct Keymap {
    xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap *keymap = nullptr;
    explicit Keymap(const char *layout, const char *options = nullptr)
    {
        xkb_rule_names names {};
        names.layout = layout;
        names.options = options;
        keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    ~Keymap()
    {
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);
    }
    uint32_t bit(const char *name) const { return 1u << xkb_keymap_mod_get_index(keymap, name); }
};

std::string read(const std::string &path)
{
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

// Caps Lock after a tap of the key `keycode` (xkb), from nothing locked.
bool capsAfterTap(const Keymap &map, xkb_keycode_t keycode)
{
    xkb_state *state = xkb_state_new(map.keymap);
    xkb_state_update_key(state, keycode, XKB_KEY_DOWN);
    xkb_state_update_key(state, keycode, XKB_KEY_UP);
    bool caps = false;
    bool num = false;
    vela_locks_read(map.keymap, xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED), &caps, &num);
    xkb_state_unref(state);
    return caps;
}

} // namespace

// The Caps Lock display follows the Lock state, not the key (§8.3): with
// the key mapped to something else Lock never changes, so nothing shows;
// with another key mapped to Caps Lock, it does.
TEST(LockKeys, TheDisplayFollowsLockNotTheKey)
{
    const xkb_keycode_t capsKey = 66; // KEY_CAPSLOCK + 8
    const xkb_keycode_t escKey = 9; // KEY_ESC + 8
    EXPECT_TRUE(capsAfterTap(Keymap("us"), capsKey));
    for (const char *options : { "caps:ctrl_modifier", "caps:escape", "caps:backspace", "caps:none", "compose:caps" }) {
        Keymap map("us", options);
        ASSERT_NE(map.keymap, nullptr) << options;
        EXPECT_FALSE(capsAfterTap(map, capsKey)) << options;
    }
    Keymap swapped("us", "caps:swapescape");
    ASSERT_NE(swapped.keymap, nullptr);
    EXPECT_FALSE(capsAfterTap(swapped, capsKey));
    EXPECT_TRUE(capsAfterTap(swapped, escKey));
}

TEST(LockKeys, ParsesTheSetting)
{
    EXPECT_EQ(vela_numlock_parse("on"), VELA_NUMLOCK_ON);
    EXPECT_EQ(vela_numlock_parse("last"), VELA_NUMLOCK_LAST);
    EXPECT_EQ(vela_numlock_parse("off"), VELA_NUMLOCK_OFF);
    EXPECT_EQ(vela_numlock_parse("On"), VELA_NUMLOCK_OFF);
    EXPECT_EQ(vela_numlock_parse(""), VELA_NUMLOCK_OFF);
    EXPECT_EQ(vela_numlock_parse(nullptr), VELA_NUMLOCK_OFF);
}

TEST(LockKeys, ReadsAndSetsOnlyCapsAndNumByName)
{
    Keymap it("it");
    ASSERT_NE(it.keymap, nullptr);
    const uint32_t caps = it.bit(XKB_MOD_NAME_CAPS);
    const uint32_t num = it.bit(XKB_MOD_NAME_NUM);
    const uint32_t shift = it.bit(XKB_MOD_NAME_SHIFT); // a sticky key's lock
    bool c = true;
    bool n = true;
    vela_locks_read(it.keymap, 0, &c, &n);
    EXPECT_FALSE(c);
    EXPECT_FALSE(n);
    vela_locks_read(it.keymap, caps | shift, &c, &n);
    EXPECT_TRUE(c);
    EXPECT_FALSE(n);
    EXPECT_EQ(vela_locks_apply(it.keymap, shift, true, true), shift | caps | num);
    EXPECT_EQ(vela_locks_apply(it.keymap, shift | caps | num, false, true), shift | num);
    EXPECT_EQ(vela_locks_apply(it.keymap, shift | caps | num, false, false), shift);
    // Another keyboard's keymap: its own bits for the same names.
    Keymap de("de");
    ASSERT_NE(de.keymap, nullptr);
    vela_locks_read(de.keymap, vela_locks_apply(de.keymap, 0, true, true), &c, &n);
    EXPECT_TRUE(c);
    EXPECT_TRUE(n);
    // No keymap: nothing is locked, nothing changes.
    vela_locks_read(nullptr, ~0u, &c, &n);
    EXPECT_FALSE(c);
    EXPECT_FALSE(n);
    EXPECT_EQ(vela_locks_apply(nullptr, shift, true, true), shift);
}

TEST(LockKeys, KeepsNumLockAtomically)
{
    const std::string root = vela::test::privateTempDirectory("numlock");
    // The directory is created, private.
    const std::string path = root + "/state/vela/numlock";
    EXPECT_FALSE(vela_numlock_load(path.c_str()));
    ASSERT_TRUE(vela_numlock_save(path.c_str(), true));
    EXPECT_EQ(read(path), "on\n");
    struct stat info { };
    ASSERT_EQ(stat((root + "/state/vela").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700);
    EXPECT_FALSE(std::filesystem::exists(path + ".new"));
    EXPECT_TRUE(vela_numlock_load(path.c_str()));
    // A restart right after a change reads it.
    ASSERT_TRUE(vela_numlock_save(path.c_str(), false));
    EXPECT_FALSE(vela_numlock_load(path.c_str()));
    // Truncated or anything else: off.
    for (const char *text : { "", "o", "on", "ON\n", "on\nmore\n", "yes\n" }) {
        std::ofstream(path) << text;
        EXPECT_FALSE(vela_numlock_load(path.c_str())) << text;
    }
    // A directory in the way: the save fails and leaves nothing behind.
    const std::string blocked = root + "/blocked";
    std::ofstream(blocked) << "a file";
    EXPECT_FALSE(vela_numlock_save((blocked + "/numlock").c_str(), true));
    EXPECT_FALSE(vela_numlock_save(nullptr, true));
    EXPECT_FALSE(vela_numlock_save("numlock", true));
}

TEST(LockKeys, StartsAsTheSettingSays)
{
    const std::string root = vela::test::privateTempDirectory("numlock-initial");
    const std::string path = root + "/numlock";
    EXPECT_FALSE(vela_numlock_initial(VELA_NUMLOCK_OFF, path.c_str()));
    EXPECT_TRUE(vela_numlock_initial(VELA_NUMLOCK_ON, path.c_str()));
    EXPECT_FALSE(vela_numlock_initial(VELA_NUMLOCK_LAST, path.c_str())); // nothing kept yet
    ASSERT_TRUE(vela_numlock_save(path.c_str(), true));
    EXPECT_TRUE(vela_numlock_initial(VELA_NUMLOCK_LAST, path.c_str()));
    EXPECT_FALSE(vela_numlock_initial(VELA_NUMLOCK_OFF, path.c_str()));
}

TEST(LockKeys, PathFollowsXdgStateHome)
{
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    const std::string savedState = state ? state : "";
    const std::string savedHome = home ? home : "";
    char path[256];
    setenv("XDG_STATE_HOME", "/state", 1);
    ASSERT_TRUE(vela_numlock_path(path, sizeof(path)));
    EXPECT_STREQ(path, "/state/vela/numlock");
    setenv("XDG_STATE_HOME", "relative", 1);
    setenv("HOME", "/home/someone", 1);
    ASSERT_TRUE(vela_numlock_path(path, sizeof(path)));
    EXPECT_STREQ(path, "/home/someone/.local/state/vela/numlock");
    unsetenv("XDG_STATE_HOME");
    setenv("HOME", "relative", 1);
    EXPECT_FALSE(vela_numlock_path(path, sizeof(path)));
    setenv("HOME", "/home/someone", 1);
    EXPECT_FALSE(vela_numlock_path(path, 8));
    state ? setenv("XDG_STATE_HOME", savedState.c_str(), 1) : unsetenv("XDG_STATE_HOME");
    home ? setenv("HOME", savedHome.c_str(), 1) : unsetenv("HOME");
}
