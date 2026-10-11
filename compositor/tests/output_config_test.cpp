// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// ~/.config/vela/outputs.conf (output_config.c): what the user chose for each
// monitor, read again in the next session; the old Italian schermi.conf. In a
// temporary directory (XDG_CONFIG_HOME).

extern "C" {
#include "output_config.h"
}

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace {

class OutputConfig : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "vela-outputs-test-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        m_dir = pattern;
        setenv("XDG_CONFIG_HOME", (m_dir + "/config").c_str(), 1);
    }
    void TearDown() override { std::filesystem::remove_all(m_dir); }

    std::string path(const char* name) const { return m_dir + "/config/vela/" + name; }
    std::string contents(const char* name) const
    {
        std::ifstream in(path(name));
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
    void write(const char* name, const std::string& text) const
    {
        std::filesystem::create_directories(m_dir + "/config/vela");
        std::ofstream(path(name)) << text;
    }

    std::string m_dir;
};

vela_output_config_entry entry(const char* key, bool enabled, int x, int y)
{
    return { key, enabled, 2560, 1440, 179960, 1.25f, 1, x, y };
}

} // namespace

TEST_F(OutputConfig, UnknownMonitor)
{
    vela_saved_output saved {};
    EXPECT_FALSE(vela_output_config_load("Dell U2720Q 123", &saved));
    write("outputs.conf", "[Other]\nenabled=yes\n");
    EXPECT_FALSE(vela_output_config_load("Dell U2720Q 123", &saved));
}

TEST_F(OutputConfig, SaveAndLoad)
{
    const vela_output_config_entry entries[] = { entry("Dell U2720Q 123", true, -2048, 0) };
    vela_output_config_save(entries, 1); // also creates ~/.config and ~/.config/vela
    EXPECT_EQ(contents("outputs.conf"),
        "# Vela's outputs, written by Vela when you change the configuration.\n"
        "# One monitor per section: [make model serial number].\n"
        "\n[Dell U2720Q 123]\nenabled=yes\nmode=2560x1440@179.960\nscale=1.25\nrotation=90\nposition=-2048,0\n");
    vela_saved_output saved {};
    ASSERT_TRUE(vela_output_config_load("Dell U2720Q 123", &saved));
    EXPECT_TRUE(saved.enabled);
    EXPECT_EQ(saved.width, 2560);
    EXPECT_EQ(saved.height, 1440);
    EXPECT_EQ(saved.refresh_mhz, 179960);
    EXPECT_FLOAT_EQ(saved.scale, 1.25f);
    EXPECT_EQ(saved.transform, 1);
    EXPECT_TRUE(saved.has_position);
    EXPECT_EQ(saved.x, -2048);
    EXPECT_EQ(saved.y, 0);
    EXPECT_FALSE(std::filesystem::exists(path("outputs.conf.tmp")));
}

// The other monitors stay as they were, in the same order; one turned off
// remembers its previous mode and position.
TEST_F(OutputConfig, KeepsOtherMonitorsAndTurnedOffSettings)
{
    write("outputs.conf", "# a comment\n[Laptop]\nenabled=yes\nmode=1920x1200@60.000\nscale=1.5\n"
                          "rotation=normal\nposition=0,0\nunknown=key\n\n[TV]\nenabled=yes\nscale=2\n");
    const vela_output_config_entry entries[] = { entry("TV", false, 0, 0), entry("New", true, 10, 20) };
    vela_output_config_save(entries, 2);
    EXPECT_EQ(contents("outputs.conf"),
        "# Vela's outputs, written by Vela when you change the configuration.\n"
        "# One monitor per section: [make model serial number].\n"
        "\n[Laptop]\nenabled=yes\nmode=1920x1200@60.000\nscale=1.5\nrotation=normal\nposition=0,0\n"
        "\n[TV]\nenabled=no\nscale=2\n"
        "\n[New]\nenabled=yes\nmode=2560x1440@179.960\nscale=1.25\nrotation=90\nposition=10,20\n");
    vela_saved_output tv {};
    ASSERT_TRUE(vela_output_config_load("TV", &tv));
    EXPECT_FALSE(tv.enabled);
    EXPECT_FLOAT_EQ(tv.scale, 2.0f);
    EXPECT_EQ(tv.width, 0); // mode not saved
    EXPECT_FALSE(tv.has_position);
}

TEST_F(OutputConfig, MissingKeysHaveDefaults)
{
    write("outputs.conf", "[Monitor]\nmode=1920x1080\nrotation=sideways\n");
    vela_saved_output saved {};
    ASSERT_TRUE(vela_output_config_load("Monitor", &saved));
    EXPECT_TRUE(saved.enabled);
    EXPECT_EQ(saved.width, 1920);
    EXPECT_EQ(saved.height, 1080);
    EXPECT_EQ(saved.refresh_mhz, 0);
    EXPECT_EQ(saved.scale, 0.0f);
    EXPECT_EQ(saved.transform, 0); // unknown name: normal
    EXPECT_FALSE(saved.has_position);
}

// Before October 2026: schermi.conf, in Italian. It's read, and becomes
// outputs.conf at the first write.
TEST_F(OutputConfig, LegacyItalianFile)
{
    write("schermi.conf", "[Vecchio]\nattivo=sì\nmodo=3840x2160@60.000\nscala=1.75\nrotazione=specchio-90\n"
                          "posizione=100,200\n");
    vela_saved_output saved {};
    ASSERT_TRUE(vela_output_config_load("Vecchio", &saved));
    EXPECT_TRUE(saved.enabled);
    EXPECT_EQ(saved.width, 3840);
    EXPECT_FLOAT_EQ(saved.scale, 1.75f);
    EXPECT_EQ(saved.transform, 5); // flipped-90
    EXPECT_EQ(saved.x, 100);
    EXPECT_EQ(saved.y, 200);

    const vela_output_config_entry entries[] = { entry("Altro", true, 0, 0) };
    vela_output_config_save(entries, 1);
    EXPECT_FALSE(std::filesystem::exists(path("schermi.conf")));
    EXPECT_NE(contents("outputs.conf").find("[Vecchio]\nenabled=yes\nmode=3840x2160@60.000\nscale=1.75\n"
                                            "rotation=flipped-90\nposition=100,200\n"),
        std::string::npos);
}

TEST_F(OutputConfig, WritesPrivateModeEvenWithPermissiveUmask)
{
    const vela_output_config_entry entries[] = { entry("Monitor", true, 0, 0) };
    const mode_t previous = umask(0);
    vela_output_config_save(entries, 1);
    umask(previous);
    struct stat info {};
    ASSERT_EQ(stat(path("outputs.conf").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
}

TEST_F(OutputConfig, DoesNotFollowPredictableTemporarySymlink)
{
    const std::string sentinel = m_dir + "/do-not-change";
    std::ofstream(sentinel) << "untouched";
    std::filesystem::create_directories(m_dir + "/config/vela");
    std::filesystem::create_symlink(sentinel, path("outputs.conf.tmp"));
    const vela_output_config_entry entries[] = { entry("Monitor", true, 0, 0) };
    vela_output_config_save(entries, 1);
    EXPECT_NE(contents("outputs.conf").find("[Monitor]"), std::string::npos);
    std::ifstream in(sentinel);
    std::string text;
    in >> text;
    EXPECT_EQ(text, "untouched");
    EXPECT_TRUE(std::filesystem::is_symlink(path("outputs.conf.tmp")));
}
