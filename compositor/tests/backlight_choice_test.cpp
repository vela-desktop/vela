// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Which backlight drives a connector (backlight_choice.c, docs/brightness.md
// §5.1), on sysfs trees shaped like the kernel's: i915 hangs its backlight
// under the connector, amdgpu under the GPU's PCI device, ACPI elsewhere.

extern "C" {
#include "backlight_choice.h"
}

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct Tree {
    fs::path root;

    Tree()
    {
        char pattern[] = "/tmp/vela-sysfs-XXXXXX";
        root = mkdtemp(pattern);
        // card1 on the GPU at 0000:00:02.0, with an eDP and a DP connector.
        fs::path gpu = root / "devices/pci0000:00/0000:00:02.0";
        fs::create_directories(gpu / "drm/card1/card1-eDP-1");
        fs::create_directories(gpu / "drm/card1/card1-DP-1");
        fs::create_directory_symlink("../..", gpu / "drm/card1/device");
        fs::create_directories(root / "devices/acpi/LNXVIDEO:00");
        fs::create_directories(root / "class/drm");
        fs::create_directories(root / "class/backlight");
        link("class/drm/card1", "devices/pci0000:00/0000:00:02.0/drm/card1");
        link("class/drm/card1-eDP-1", "devices/pci0000:00/0000:00:02.0/drm/card1/card1-eDP-1");
    }
    ~Tree() { fs::remove_all(root); }

    void link(const std::string& from, const std::string& to) const
    {
        fs::create_directory_symlink(fs::relative(root / to, (root / from).parent_path()), root / from);
    }

    // A backlight device whose parent is `parent` (a path in the tree).
    void backlight(const std::string& name, const char* type, const std::string& parent) const
    {
        fs::path dir = root / "class/backlight" / name;
        fs::create_directories(dir);
        std::ofstream(dir / "type") << type << "\n";
        fs::create_directory_symlink(fs::relative(root / parent, dir), dir / "device");
    }

    std::string choose(const char* connector) const
    {
        char device[64] = "";
        return vela_backlight_choose(root.c_str(), connector, device, sizeof(device)) ? device : "(none)";
    }
};

const std::string connector = "devices/pci0000:00/0000:00:02.0/drm/card1/card1-eDP-1";
const std::string gpu = "devices/pci0000:00/0000:00:02.0";
const std::string acpi = "devices/acpi/LNXVIDEO:00";

TEST(BacklightChoice, TheConnectorsOwnComesFirst)
{
    Tree tree;
    tree.backlight("acpi_video0", "firmware", acpi);
    tree.backlight("intel_backlight", "raw", connector);
    EXPECT_EQ(tree.choose("card1-eDP-1"), "intel_backlight");
}

TEST(BacklightChoice, ThenOneOnTheSameCard)
{
    Tree tree;
    tree.backlight("acpi_video0", "firmware", acpi);
    tree.backlight("amdgpu_bl1", "raw", gpu); // under the PCI device, like amdgpu's
    EXPECT_EQ(tree.choose("card1-eDP-1"), "amdgpu_bl1");
}

TEST(BacklightChoice, ByTypeAmongEquals)
{
    Tree tree;
    tree.backlight("raw_one", "raw", gpu);
    tree.backlight("platform_one", "platform", gpu);
    tree.backlight("firmware_one", "firmware", gpu);
    EXPECT_EQ(tree.choose("card1-eDP-1"), "firmware_one");

    Tree names;
    names.backlight("b", "raw", acpi);
    names.backlight("a", "raw", acpi);
    EXPECT_EQ(names.choose("card1-eDP-1"), "a");
}

TEST(BacklightChoice, ABuiltInScreenTakesAnyAsALastResort)
{
    Tree tree;
    tree.backlight("acpi_video0", "firmware", acpi);
    EXPECT_EQ(tree.choose("card1-eDP-1"), "acpi_video0");
    EXPECT_EQ(tree.choose("card1-LVDS-1"), "acpi_video0");
}

TEST(BacklightChoice, AnExternalMonitorIsDdcs)
{
    Tree tree;
    tree.backlight("acpi_video0", "firmware", acpi);
    tree.backlight("amdgpu_bl1", "raw", gpu);
    EXPECT_EQ(tree.choose("card1-DP-1"), "(none)");
    // ... unless the backlight hangs under that very connector.
    tree.backlight("dp_backlight", "raw", "devices/pci0000:00/0000:00:02.0/drm/card1/card1-DP-1");
    EXPECT_EQ(tree.choose("card1-DP-1"), "dp_backlight");
}

TEST(BacklightChoice, NoBacklightsAtAll)
{
    Tree tree;
    EXPECT_EQ(tree.choose("card1-eDP-1"), "(none)");
    fs::remove_all(tree.root / "class/backlight");
    EXPECT_EQ(tree.choose("card1-eDP-1"), "(none)");
}

TEST(BacklightChoice, BuiltInConnectors)
{
    for (const char* name : { "card0-eDP-1", "eDP-2", "card12-LVDS-1", "DSI-1" }) {
        EXPECT_TRUE(vela_connector_is_internal(name)) << name;
    }
    for (const char* name : { "card0-DP-1", "HDMI-A-1", "card1-DP-eDP-1", "cardx-eDP-1", "HEADLESS-1", "" }) {
        EXPECT_FALSE(vela_connector_is_internal(name)) << name;
    }
}

} // namespace
