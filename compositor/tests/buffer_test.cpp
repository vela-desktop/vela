// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Growing text (buffer.c), used to build the messages to the shell and the
// state JSON, and the session's desktop names (util.c).

extern "C" {
#include "buffer.h"
#include "util.h"
}

#include <gtest/gtest.h>

#include <cstdint>
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

TEST(Buffer, GrowsThroughTheSizeCore)
{
    // vela_grow's size_t form: 8 bytes at first, then doubling, tail zeroed.
    vela_buffer buffer = {};
    vela_buffer_append(&buffer, "1234567");
    EXPECT_EQ(buffer.capacity, 8u);
    vela_buffer_append(&buffer, "8");
    EXPECT_EQ(buffer.capacity, 16u);
    for (size_t i = buffer.length + 1; i < buffer.capacity; ++i) {
        EXPECT_EQ(buffer.data[i], '\0');
    }
    vela_buffer_append(&buffer, std::string(40, 'x').c_str());
    EXPECT_EQ(buffer.capacity, 64u);
    EXPECT_EQ(text(buffer), "12345678" + std::string(40, 'x'));
    vela_buffer_finish(&buffer);
}

TEST(BufferDeathTest, LengthOverflowAborts)
{
    vela_buffer buffer = {};
    vela_buffer_append(&buffer, "x");
    size_t length = buffer.length;
    // A length that the next append would wrap: the addition is checked.
    buffer.length = SIZE_MAX - 1;
    EXPECT_DEATH(vela_buffer_append(&buffer, "ab"), "vela: size overflow \\(18446744073709551614 \\+ 2\\) at .*buffer\\.c:");
    buffer.length = length;
    vela_buffer_finish(&buffer);
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

} // namespace
