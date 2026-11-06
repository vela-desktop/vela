// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Growing text (buffer.c), used to build the messages to the shell and the
// state JSON, and the session's desktop names (util.c).

extern "C" {
#include "buffer.h"
#include "util.h"
}

#include <gtest/gtest.h>

#include <string>

namespace {

std::string text(const vela_buffer& buffer)
{
    return vela_buffer_text(&buffer);
}

TEST(Buffer, StartsEmptyAndGrows)
{
    vela_buffer buffer = {};
    EXPECT_EQ(text(buffer), "");
    vela_buffer_append(&buffer, "switcher-");
    vela_buffer_appendf(&buffer, "%s %d", "show", 2);
    EXPECT_EQ(text(buffer), "switcher-show 2");
    EXPECT_EQ(buffer.length, 15u);
    // Much more than the initial capacity.
    std::string expected = text(buffer);
    for (int i = 0; i < 1000; ++i) {
        vela_buffer_appendf(&buffer, " %d", i);
        expected += " " + std::to_string(i);
    }
    EXPECT_EQ(text(buffer), expected);
    vela_buffer_finish(&buffer);
    EXPECT_EQ(buffer.data, nullptr);
    EXPECT_EQ(buffer.length, 0u);
}

TEST(Buffer, JsonStringsAreEscaped)
{
    vela_buffer buffer = {};
    vela_buffer_append_json(&buffer, "Desktop \"1\"\\ok\n\tend\x01");
    EXPECT_EQ(text(buffer), "\"Desktop \\\"1\\\"\\\\ok\\n\\tend\\u0001\"");
    vela_buffer_finish(&buffer);
    vela_buffer_append_json(&buffer, nullptr);
    vela_buffer_append_json(&buffer, "è");
    EXPECT_EQ(text(buffer), "\"\"\"è\"");
    vela_buffer_finish(&buffer);
}

TEST(DesktopNames, SeparatedByColons)
{
    char out[64];
    ASSERT_TRUE(vela_desktop_names("Vela;KDE;", out, sizeof(out)));
    EXPECT_STREQ(out, "Vela:KDE");
    ASSERT_TRUE(vela_desktop_names("Vela:KDE", out, sizeof(out)));
    EXPECT_STREQ(out, "Vela:KDE");
    ASSERT_TRUE(vela_desktop_names(";;KDE;;Vela", out, sizeof(out)));
    EXPECT_STREQ(out, "KDE:Vela");
    ASSERT_TRUE(vela_desktop_names("", out, sizeof(out)));
    EXPECT_STREQ(out, "");
    ASSERT_TRUE(vela_desktop_names(nullptr, out, sizeof(out)));
    EXPECT_STREQ(out, "");
    EXPECT_FALSE(vela_desktop_names("Vela;KDE", out, 5));
}

TEST(SecretBackend, KeepsTheStoreThatAlreadyHasSecrets)
{
    EXPECT_STREQ(vela_secret_backend(56000, 826), "GNOME");
    EXPECT_STREQ(vela_secret_backend(56000, 0), "GNOME");
    EXPECT_STREQ(vela_secret_backend(0, 40000), "KDE");
    EXPECT_STREQ(vela_secret_backend(0, 0), "KDE");
    EXPECT_STREQ(vela_secret_backend(200, 40000), "KDE");
}

} // namespace
