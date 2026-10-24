// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Finding a connector's bus (i2c.h), on a sysfs made of directories and
// links as amdgpu lays them out (docs/brightness.md §5.1).

extern "C" {
#include "i2c.h"
}

#include <gtest/gtest.h>

#include <cerrno>
#include <filesystem>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

class Sysfs : public ::testing::Test {
protected:
    void SetUp() override
    {
        root = fs::path(::testing::TempDir()) / ("vela-sysfs-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "class/drm");
        fs::create_directories(root / "devices/i2c-5");
        fs::create_directories(root / "devices/i2c-6");
    }
    void TearDown() override { fs::remove_all(root); }

    fs::path connector(const std::string& name)
    {
        fs::path dir = root / "class/drm" / name;
        fs::create_directories(dir);
        return dir;
    }

    int find(const char* name, std::string& bus, std::string& how)
    {
        char out[32] = "";
        const char* via = nullptr;
        int result = vela_i2c_find_bus(root.c_str(), name, out, sizeof(out), &via);
        bus = out;
        how = via ? via : "";
        return result;
    }

    fs::path root;
};

} // namespace

TEST_F(Sysfs, DisplayPortUsesItsAuxBusNotTheDdcLink)
{
    // DP-3: AUX is i2c-9, and the ddc link points at i2c-5, which doesn't
    // reach the monitor.
    fs::path dp = connector("card1-DP-3");
    fs::create_directories(dp / "i2c-9");
    fs::create_directory_symlink("../../../devices/i2c-5", dp / "ddc");
    std::string bus, how;
    ASSERT_EQ(find("card1-DP-3", bus, how), 0);
    EXPECT_EQ(bus, "i2c-9");
    EXPECT_EQ(how, "aux");
}

TEST_F(Sysfs, HdmiUsesItsDdcLink)
{
    fs::path hdmi = connector("card1-HDMI-A-1");
    fs::create_directory_symlink("../../../devices/i2c-6", hdmi / "ddc");
    std::string bus, how;
    ASSERT_EQ(find("card1-HDMI-A-1", bus, how), 0);
    EXPECT_EQ(bus, "i2c-6");
    EXPECT_EQ(how, "ddc");
}

TEST_F(Sysfs, LowestAuxBusWins)
{
    fs::path dp = connector("card0-DP-1");
    fs::create_directories(dp / "i2c-12");
    fs::create_directories(dp / "i2c-10");
    fs::create_directories(dp / "i2c-x"); // not a bus
    std::string bus, how;
    ASSERT_EQ(find("card0-DP-1", bus, how), 0);
    EXPECT_EQ(bus, "i2c-10");
}

TEST_F(Sysfs, NoBus)
{
    connector("card1-DP-1");
    std::string bus, how;
    EXPECT_EQ(find("card1-DP-1", bus, how), -ENOENT);
    EXPECT_EQ(find("card1-DP-9", bus, how), -ENOENT); // no such connector
    fs::path odd = connector("card1-VGA-1");
    fs::create_directory_symlink("../../../devices/not-a-bus", odd / "ddc");
    EXPECT_EQ(find("card1-VGA-1", bus, how), -ENOENT);
}

TEST_F(Sysfs, RejectsPathsAsConnectors)
{
    std::string bus, how;
    for (const char* name : { "", ".", "..", "../drm", "card1-DP-3/..", "/etc" }) {
        EXPECT_EQ(find(name, bus, how), -EINVAL) << name;
    }
}
