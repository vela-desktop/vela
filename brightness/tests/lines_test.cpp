// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The lines between the compositor and vela-brightness (lines.h).

extern "C" {
#include "lines.h"
}

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

std::string format(vela_event event)
{
    char line[VELA_LINE_MAX];
    size_t length = vela_format_event(line, sizeof(line), &event);
    return std::string(line, length);
}

std::vector<std::string> split(vela_line_reader& reader, const std::string& bytes)
{
    std::vector<std::string> lines;
    vela_line_reader_feed(&reader, bytes.data(), bytes.size(),
        [](void* data, const char* line) { static_cast<std::vector<std::string>*>(data)->push_back(line); },
        &lines);
    return lines;
}

} // namespace

TEST(Lines, ParsesEveryCommand)
{
    vela_command command = vela_parse_command("set 7 80");
    EXPECT_EQ(command.kind, VELA_COMMAND_SET);
    EXPECT_EQ(command.seq, 7u);
    EXPECT_EQ(command.value, 80u);

    command = vela_parse_command("set 18446744073709551615 65535");
    EXPECT_EQ(command.kind, VELA_COMMAND_SET);
    EXPECT_EQ(command.seq, UINT64_MAX);
    EXPECT_EQ(command.value, 65535u);

    command = vela_parse_command("live on");
    EXPECT_EQ(command.kind, VELA_COMMAND_LIVE);
    EXPECT_TRUE(command.on);
    command = vela_parse_command("live off");
    EXPECT_FALSE(command.on);

    command = vela_parse_command("power off");
    EXPECT_EQ(command.kind, VELA_COMMAND_POWER);
    EXPECT_FALSE(command.on);

    command = vela_parse_command("session active");
    EXPECT_EQ(command.kind, VELA_COMMAND_SESSION);
    EXPECT_TRUE(command.on);
    command = vela_parse_command("session inactive");
    EXPECT_FALSE(command.on);

    EXPECT_EQ(vela_parse_command("probe").kind, VELA_COMMAND_PROBE);
}

TEST(Lines, RejectsMalformedCommands)
{
    for (const char* line : {
             "", "set", "set 1", "set 0 50", "set 1 65536", "set 1 -5", "set -1 5", "set 1 5 ", "set  1 5",
             "set 1  5", "set 18446744073709551616 5", "set 1 5x", "set x 5", "live", "live maybe", "live on ",
             "power", "power active", "session on", "probe now", " probe", "brightness 5" }) {
        EXPECT_EQ(vela_parse_command(line).kind, VELA_COMMAND_INVALID) << '"' << line << '"';
    }
}

TEST(Lines, FormatsEveryEvent)
{
    EXPECT_EQ(format({ VELA_EVENT_STATE, VELA_STATE_READY, 0, 80, 100 }), "state ready 80 100\n");
    EXPECT_EQ(format({ VELA_EVENT_STATE, VELA_STATE_NO_ANSWER, 0, 0, 0 }), "state no-answer\n");
    EXPECT_EQ(format({ VELA_EVENT_STATE, VELA_STATE_NOT_RESPONDING, 0, 0, 0 }), "state not-responding\n");
    EXPECT_EQ(format({ VELA_EVENT_STATE, VELA_STATE_UNSUPPORTED, 0, 0, 0 }), "state unsupported\n");
    EXPECT_EQ(format({ VELA_EVENT_STATE, VELA_STATE_NO_ACCESS, 0, 0, 0 }), "state no-access\n");
    EXPECT_EQ(format({ VELA_EVENT_STATE, VELA_STATE_NO_BUS, 0, 0, 0 }), "state no-bus\n");
    EXPECT_EQ(format({ VELA_EVENT_VALUE, VELA_STATE_READY, 7, 80, 100 }), "value 7 80\n");
    EXPECT_EQ(format({ VELA_EVENT_REFUSED, VELA_STATE_READY, 3, 40, 100 }), "refused 3 40\n");
}

TEST(Lines, ReaderSplitsAcrossReads)
{
    vela_line_reader reader {};
    EXPECT_TRUE(split(reader, "set 1 ").empty());
    EXPECT_EQ(split(reader, "50\nset 2 60\nli"), (std::vector<std::string> { "set 1 50", "set 2 60" }));
    EXPECT_EQ(split(reader, "ve on\n"), (std::vector<std::string> { "live on" }));
}

TEST(Lines, ReaderDropsOverlongLinesWhole)
{
    vela_line_reader reader {};
    std::string flood(VELA_LINE_MAX + 50, 'x');
    EXPECT_TRUE(split(reader, flood).empty());
    EXPECT_EQ(split(reader, "tail\nprobe\n"), (std::vector<std::string> { "probe" }));
    // Exactly the limit still fits.
    std::string longest(VELA_LINE_MAX, 'y');
    EXPECT_EQ(split(reader, longest + "\n"), (std::vector<std::string> { longest }));
}
