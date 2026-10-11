// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// ~/.config/vela/vela.conf (config.c): reading, writing that keeps the rest,
// yes/no values. In a temporary directory (XDG_CONFIG_HOME).

extern "C" {
#include "config.h"
}

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace {

class Settings : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "vela-settings-test-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        m_dir = pattern;
        setenv("XDG_CONFIG_HOME", m_dir.c_str(), 1);
    }
    void TearDown() override { std::filesystem::remove_all(m_dir); }

    std::string file() const { return m_dir + "/vela/vela.conf"; }
    std::string contents() const
    {
        std::ifstream in(file());
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
    void write(const std::string& text) const
    {
        std::filesystem::create_directories(m_dir + "/vela");
        std::ofstream(file()) << text;
    }

    std::string m_dir;
};

// vela.conf as read now; freed automatically.
struct Read {
    vela_config config;
    Read() { vela_config_read(&config); }
    ~Read() { vela_config_finish(&config); }
    const char* get(const char* key, const char* fallback = "") const
    {
        return vela_config_get(&config, key, fallback);
    }
    bool flag(const char* key, bool fallback) const { return vela_config_flag(&config, key, fallback); }
};

} // namespace

TEST_F(Settings, MissingFileIsEmpty)
{
    EXPECT_EQ(Read().config.count, 0);
    EXPECT_STREQ(Read().get("tearing", "yes"), "yes");
}

TEST_F(Settings, ReadIgnoresCommentsAndJunk)
{
    write("# comment\nnight-light=yes\nline without equals\nstrength=40\n=empty\n");
    const Read settings;
    EXPECT_STREQ(settings.get("night-light"), "yes");
    EXPECT_STREQ(settings.get("strength"), "40");
    EXPECT_EQ(settings.get("line without equals", nullptr), nullptr);
}

TEST_F(Settings, WriteCreatesUpdatesAndKeepsTheRest)
{
    vela_config_write("tearing", "no");
    EXPECT_STREQ(Read().get("tearing"), "no");
    EXPECT_EQ(contents().rfind('#', 0), 0u); // a new file has the header line

    write("# my comment\nscreen-off=10\ntearing=yes\ntearing=duplicate\n");
    vela_config_write("tearing", "no");
    vela_config_write("variable-refresh", "always");
    EXPECT_EQ(contents(), "# my comment\nscreen-off=10\ntearing=no\nvariable-refresh=always\n");
    EXPECT_FALSE(std::filesystem::exists(file() + ".tmp")); // written with a rename
}

TEST_F(Settings, Flags)
{
    write("c=1\nd=true\ne=yes\nf=no\ng=0\nh=\n");
    const Read s;
    for (const char* key : { "c", "d", "e" }) {
        EXPECT_TRUE(s.flag(key, false)) << key;
    }
    EXPECT_FALSE(s.flag("f", true));
    EXPECT_FALSE(s.flag("g", true));
    EXPECT_TRUE(s.flag("h", true)); // empty: the default
    EXPECT_TRUE(s.flag("missing", true));
}

// Files written before October 2026 have Italian keys and values.
TEST_F(Settings, LegacyItalianNames)
{
    write("# commento\nluce-notturna=sì\nluce-notturna-pianifica=tramonto\nfrequenza-variabile=giochi\n"
          "mouse-pulsante-principale=destro\nfiltro-colore=grigi\ntouchpad-tocco=si\nlingua=it\n");
    const Read s;
    EXPECT_TRUE(s.flag("night-light", false));
    EXPECT_STREQ(s.get("night-light-schedule"), "sunset");
    EXPECT_STREQ(s.get("variable-refresh"), "games");
    EXPECT_STREQ(s.get("mouse-primary-button"), "right");
    EXPECT_STREQ(s.get("color-filter"), "grayscale");
    EXPECT_TRUE(s.flag("touchpad-tap", false));
    EXPECT_STREQ(s.get("language"), "it");

    // At startup the file is rewritten with the new names; comments stay.
    vela_config_migrate();
    EXPECT_EQ(contents(), "# commento\nnight-light=yes\nnight-light-schedule=sunset\nvariable-refresh=games\n"
                          "mouse-primary-button=right\ncolor-filter=grayscale\ntouchpad-tap=yes\nlanguage=it\n");
}

TEST_F(Settings, WritesPrivateModeEvenWithPermissiveUmask)
{
    const mode_t previous = umask(0);
    vela_config_write("night-light", "yes");
    umask(previous);
    struct stat info {};
    ASSERT_EQ(stat(file().c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
}

TEST_F(Settings, DoesNotFollowPredictableTemporarySymlink)
{
    std::filesystem::create_directories(m_dir + "/vela");
    const std::string sentinel = m_dir + "/do-not-change";
    std::ofstream(sentinel) << "untouched";
    std::filesystem::create_symlink(sentinel, file() + ".tmp");
    vela_config_write("night-light", "yes");
    EXPECT_STREQ(Read().get("night-light"), "yes");
    std::ifstream in(sentinel);
    std::string text;
    in >> text;
    EXPECT_EQ(text, "untouched");
    EXPECT_TRUE(std::filesystem::is_symlink(file() + ".tmp"));
}
