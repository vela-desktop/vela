// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The built-in backlight (backlight.c, docs/brightness.md §5.3) on a fake
// sysfs tree: what is read, the minimum, the OLED guard, and the helper's
// lines with a backlight's timing.

extern "C" {
#include "backlight.h"
#include "helper.h"
}

#include "fake_clock.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Tree {
    fs::path root;
    fs::path dir;

    explicit Tree(const char* device = "intel_backlight")
    {
        char pattern[] = "/tmp/vela-backlight-XXXXXX";
        root = mkdtemp(pattern);
        dir = root / "class" / "backlight" / device;
        fs::create_directories(dir);
    }
    ~Tree() { fs::remove_all(root); }

    void write(const char* name, const std::string& text) const { std::ofstream(dir / name) << text; }
    std::string read(const char* name) const
    {
        std::ifstream file(dir / name);
        std::string text;
        std::getline(file, text);
        return text;
    }
};

// A panel: `set` records the values; at or below `dark` it shows nothing
// (actual_brightness 0), like some OLED panels.
struct Panel {
    Tree tree;
    FakeClock clock;
    vela_backlight backlight {};
    std::vector<uint32_t> sets;
    int error = 0;
    uint32_t dark = 0;
    bool has_actual = true;

    explicit Panel(uint32_t max = 96000, uint32_t value = 48000)
    {
        tree.write("max_brightness", std::to_string(max) + "\n");
        tree.write("brightness", std::to_string(value) + "\n");
        if (has_actual) {
            tree.write("actual_brightness", std::to_string(value) + "\n");
        }
        EXPECT_EQ(vela_backlight_init(&backlight, tree.root.c_str(), "intel_backlight", true, &clock.clock), 0);
        backlight.set = [](void* data, const char*, const char*, uint32_t value) {
            auto* panel = static_cast<Panel*>(data);
            if (panel->error) {
                return panel->error;
            }
            panel->sets.push_back(value);
            panel->tree.write("brightness", std::to_string(value));
            if (panel->has_actual) {
                panel->tree.write("actual_brightness", std::to_string(value <= panel->dark ? 0 : value));
            }
            return 0;
        };
        backlight.set_data = this;
    }

    vela_result write(uint32_t value, uint32_t* applied) { return vela_backlight_write(&backlight, value, applied); }
};

TEST(Backlight, ReadsValueAndMaximum)
{
    Panel panel(96000, 48000);
    uint32_t value = 0, max = 0;
    EXPECT_EQ(vela_backlight_read(&panel.backlight, &value, &max), VELA_RESULT_OK);
    EXPECT_EQ(value, 48000u);
    EXPECT_EQ(max, 96000u); // more than 16 bits
}

TEST(Backlight, WhatCantBeRead)
{
    Panel panel;
    uint32_t value, max;
    panel.tree.write("max_brightness", "0\n");
    EXPECT_EQ(vela_backlight_read(&panel.backlight, &value, &max), VELA_RESULT_UNSUPPORTED);
    panel.tree.write("max_brightness", "lots\n");
    EXPECT_EQ(vela_backlight_read(&panel.backlight, &value, &max), VELA_RESULT_NO_ANSWER);
    panel.tree.write("max_brightness", "100\n");
    if (geteuid() != 0) {
        fs::permissions(panel.tree.dir / "brightness", fs::perms::none);
        EXPECT_EQ(vela_backlight_read(&panel.backlight, &value, &max), VELA_RESULT_NO_ACCESS);
        fs::permissions(panel.tree.dir / "brightness", fs::perms::owner_all);
    }
    fs::remove_all(panel.tree.dir);
    EXPECT_EQ(vela_backlight_read(&panel.backlight, &value, &max), VELA_RESULT_NO_BUS);
}

TEST(Backlight, NamesThatAreNotOneFileAreRefused)
{
    FakeClock clock;
    vela_backlight backlight;
    for (const char* bad : { "", ".", "..", "../x", "a/b" }) {
        EXPECT_EQ(vela_backlight_init(&backlight, "/sys", bad, false, &clock.clock), -EINVAL) << bad;
    }
}

// COSMIC: 0 can switch a panel off, unless the device is coarse.
TEST(Backlight, NeverZeroWithManyLevels)
{
    Panel panel(96000);
    uint32_t applied = 7;
    EXPECT_EQ(panel.write(0, &applied), VELA_RESULT_OK);
    EXPECT_EQ(applied, 1u);
    EXPECT_EQ(panel.sets, std::vector<uint32_t> { 1 });
    EXPECT_EQ(panel.write(200000, &applied), VELA_RESULT_OK);
    EXPECT_EQ(applied, 96000u);

    Panel coarse(15, 8);
    EXPECT_EQ(coarse.write(0, &applied), VELA_RESULT_OK);
    EXPECT_EQ(applied, 0u);
}

TEST(Backlight, TheOledGuardRaisesALowValueThreeTimesAtMost)
{
    Panel panel(1000);
    panel.dark = 2; // 1 and 2 show nothing
    uint32_t applied = 0;
    EXPECT_EQ(panel.write(1, &applied), VELA_RESULT_OK);
    EXPECT_EQ(panel.sets, (std::vector<uint32_t> { 1, 2, 3 }));
    EXPECT_EQ(applied, 3u);
    EXPECT_EQ(panel.tree.read("brightness"), "3");

    Panel darker(1000);
    darker.dark = 40;
    EXPECT_EQ(darker.write(1, &applied), VELA_RESULT_OK);
    EXPECT_EQ(darker.sets, (std::vector<uint32_t> { 1, 2, 3, 4 }));
    EXPECT_EQ(applied, 4u);
}

TEST(Backlight, TheOledGuardOnlyBelowFivePercent)
{
    Panel panel(1000);
    panel.dark = 100;
    uint32_t applied = 0;
    EXPECT_EQ(panel.write(51, &applied), VELA_RESULT_OK);
    EXPECT_EQ(panel.sets, std::vector<uint32_t> { 51 });
    EXPECT_EQ(applied, 51u);
}

TEST(Backlight, NoActualBrightnessNoGuard)
{
    Panel panel(1000);
    panel.has_actual = false;
    fs::remove(panel.tree.dir / "actual_brightness");
    panel.dark = 100;
    uint32_t applied = 0;
    EXPECT_EQ(panel.write(10, &applied), VELA_RESULT_OK);
    EXPECT_EQ(panel.sets, std::vector<uint32_t> { 10 });
}

TEST(Backlight, LogindSayingNo)
{
    Panel panel;
    uint32_t applied;
    panel.error = -EACCES;
    EXPECT_EQ(panel.write(500, &applied), VELA_RESULT_NO_ACCESS);
    panel.error = -ETIMEDOUT;
    EXPECT_EQ(panel.write(500, &applied), VELA_RESULT_NO_ANSWER);
}

// A shorter value replaces the old one whole, and a missing file is never
// created.
TEST(Backlight, TheFakeTreeIsWrittenInPlace)
{
    Tree tree;
    tree.write("max_brightness", "100\n");
    tree.write("brightness", "50\n");
    EXPECT_EQ(vela_backlight_set_file(nullptr, "intel_backlight", tree.dir.c_str(), 7), 0);
    std::ifstream file(tree.dir / "brightness");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "7\n");
    fs::remove(tree.dir / "brightness");
    EXPECT_EQ(vela_backlight_set_file(nullptr, "intel_backlight", tree.dir.c_str(), 30), -ENOENT);
    EXPECT_FALSE(fs::exists(tree.dir / "brightness"));
}

// The helper with a backlight: every value of a drag is written as it comes,
// and a 0 ends confirmed at the minimum.
TEST(Backlight, HelperFollowsADragAndConfirmsTheMinimum)
{
    Panel panel(96000, 48000);
    std::vector<std::string> lines;
    vela_helper helper;
    vela_helper_init_backend(&helper, vela_backlight_backend(&panel.backlight), VELA_BACKLIGHT_WRITE_INTERVAL_MS,
        VELA_BACKLIGHT_READBACK_MS, &panel.clock.clock,
        [](void* data, const char* line) { static_cast<std::vector<std::string>*>(data)->push_back(line); }, &lines);
    auto run = [&] {
        for (int i = 0; i < 100; ++i) {
            int64_t at = vela_helper_run(&helper);
            if (at == VELA_NEVER) {
                return;
            }
            if (at > panel.clock.monotonic) {
                panel.clock.advance(at - panel.clock.monotonic);
            }
        }
    };
    run();
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(std::find(lines.begin(), lines.end(), "state ready 48000 96000\n"), lines.end());
    for (uint32_t step = 1; step <= 5; ++step) {
        vela_helper_line(&helper, ("set " + std::to_string(step) + " " + std::to_string(step * 9000)).c_str());
        vela_helper_run(&helper); // at once: no interval
        EXPECT_EQ(panel.sets.back(), step * 9000);
    }
    lines.clear();
    run();
    EXPECT_EQ(lines.back(), "value 5 45000\n");

    vela_helper_line(&helper, "set 6 0");
    lines.clear();
    run();
    EXPECT_EQ(panel.sets.back(), 1u);
    EXPECT_EQ(lines.back(), "value 6 1\n");
}

} // namespace
