// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" {
#include "output_key.h"
}
#include <gtest/gtest.h>
#include <cstring>
#include <string>

TEST(OutputKey, JoinsMonitorMetadata)
{
    char out[128];
    vela_output_key_format(out, sizeof(out), "Dell", "U2720Q", "123", "DP-1");
    EXPECT_STREQ(out, "Dell U2720Q 123");
}

TEST(OutputKey, UsesFallbackWhenMetadataMissing)
{
    char out[32];
    vela_output_key_format(out, sizeof(out), nullptr, "", nullptr, "DP-1");
    EXPECT_STREQ(out, "DP-1");
}

TEST(OutputKey, HandlesZeroAndOneByteBuffers)
{
    char marker = '!';
    vela_output_key_format(&marker, 0, "Dell", "U2720Q", "123", "DP-1");
    EXPECT_EQ(marker, '!');
    vela_output_key_format(&marker, 1, "Dell", "U2720Q", "123", "DP-1");
    EXPECT_EQ(marker, '\0');
}

TEST(OutputKey, LongMetadataDoesNotOverflow)
{
    struct Buffer { char out[8]; char guard[8]; } buffer {};
    std::memset(buffer.guard, 'G', sizeof(buffer.guard));
    std::string veryLong(4096, 'X');
    vela_output_key_format(buffer.out, sizeof(buffer.out), veryLong.c_str(),
        "model", "serial", "DP-1");
    EXPECT_EQ(std::strlen(buffer.out), sizeof(buffer.out) - 1);
    EXPECT_EQ(buffer.out[sizeof(buffer.out) - 1], '\0');
    for (char c : buffer.guard) {
        EXPECT_EQ(c, 'G');
    }
}
