// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// One output's brightness in the compositor (brightness_model.c,
// docs/brightness.md §7.1): the helper's lines, the clients' requests and
// what clients are shown.

extern "C" {
#include "brightness_model.h"
#include "buffer.h"
}

#include <gtest/gtest.h>

#include <string>

namespace {

struct Model {
    vela_brightness_model model;

    Model() { vela_brightness_model_init(&model); }

    vela_brightness_line line(const char* text) { return vela_brightness_model_line(&model, text); }

    // The line for the helper, "" when only the shown state changed, "(no)"
    // when nothing changed or it is not settable.
    std::string set(double value)
    {
        char out[64];
        if (!vela_brightness_model_set(&model, value, out, sizeof(out))) {
            return "(no)";
        }
        return out;
    }

    std::string json(const char* name = "DP-3") const
    {
        vela_buffer buffer = {};
        vela_brightness_model_json(&model, name, "Mi Monitor", "ddc", &buffer);
        std::string text = vela_buffer_text(&buffer);
        vela_buffer_finish(&buffer);
        return text;
    }
};

TEST(BrightnessModel, ProbingUntilTheMonitorAnswers)
{
    Model m;
    EXPECT_EQ(m.json(),
        R"({"name":"DP-3","description":"Mi Monitor","mechanism":"ddc","state":"probing","value":null,"pending":false,"refused":false,"max":0,"seq":0})");
    EXPECT_EQ(m.set(0.5), "(no)");
    EXPECT_EQ(m.line("state ready 80 100"), VELA_BRIGHTNESS_LINE_CHANGED);
    EXPECT_EQ(m.json(),
        R"({"name":"DP-3","description":"Mi Monitor","mechanism":"ddc","state":"ready","value":0.8,"pending":false,"refused":false,"max":100,"seq":0})");
}

TEST(BrightnessModel, SetIsRoundedToTheDeviceAndShownAtOnce)
{
    Model m;
    m.line("state ready 80 100");
    EXPECT_EQ(m.set(0.404), "set 1 40\n");
    EXPECT_TRUE(m.model.pending);
    EXPECT_NE(m.json().find(R"("value":0.4,"pending":true)"), std::string::npos);
    // The same value again: nothing for the helper.
    EXPECT_EQ(m.set(0.4), "(no)");
    EXPECT_EQ(m.set(0.9), "set 2 90\n");
    EXPECT_EQ(m.line("value 2 90"), VELA_BRIGHTNESS_LINE_CHANGED);
    EXPECT_FALSE(m.model.pending);
    EXPECT_EQ(m.model.confirmed, 90u);
}

TEST(BrightnessModel, OutOfRangeValuesAreNotApplied)
{
    Model m;
    m.line("state ready 80 100");
    EXPECT_EQ(m.set(-0.1), "(no)");
    EXPECT_EQ(m.set(1.01), "(no)");
    EXPECT_EQ(m.set(0.0 / 0.0), "(no)");
    EXPECT_EQ(m.set(1.0), "set 1 100\n");
    EXPECT_EQ(m.set(0.0), "set 2 0\n");
}

// COSMIC #184: while the monitor catches up, the slider stays where the user
// put it; an answer to an older target only says what the monitor has.
TEST(BrightnessModel, AnOldAnswerDoesNotMoveTheSlider)
{
    Model m;
    m.line("state ready 80 100");
    m.set(0.4);
    m.set(0.8);
    EXPECT_EQ(m.line("value 1 40"), VELA_BRIGHTNESS_LINE_UNCHANGED);
    EXPECT_TRUE(m.model.pending);
    EXPECT_EQ(m.model.confirmed, 40u);
    EXPECT_EQ(m.model.target, 80u);
    EXPECT_NE(m.json().find(R"("value":0.8,"pending":true)"), std::string::npos);
    m.line("value 2 80");
    EXPECT_FALSE(m.model.pending);
}

TEST(BrightnessModel, LiveReadsFollowTheButtons)
{
    Model m;
    m.line("state ready 80 100");
    EXPECT_EQ(m.line("value 0 55"), VELA_BRIGHTNESS_LINE_CHANGED);
    EXPECT_NE(m.json().find(R"("value":0.55,)"), std::string::npos);
    EXPECT_EQ(m.line("value 0 55"), VELA_BRIGHTNESS_LINE_UNCHANGED);
}

TEST(BrightnessModel, RefusedPutsTheSliderBackAndKeepsTheNote)
{
    Model m;
    m.line("state ready 80 100");
    m.set(0.3);
    EXPECT_EQ(m.line("refused 1 80"), VELA_BRIGHTNESS_LINE_CHANGED);
    EXPECT_FALSE(m.model.pending);
    EXPECT_TRUE(m.model.refused);
    EXPECT_NE(m.json().find(R"("value":0.8,"pending":false,"refused":true)"), std::string::npos);
    // A live read with the newest seq answers nothing: the note stays.
    m.line("value 1 80");
    EXPECT_TRUE(m.model.refused);
    // The next change clears it, even to the value the monitor kept.
    EXPECT_EQ(m.set(0.8), "");
    EXPECT_FALSE(m.model.refused);
    m.set(0.3);
    m.line("refused 2 80");
    EXPECT_EQ(m.set(0.5), "set 3 50\n");
    EXPECT_FALSE(m.model.refused);
}

TEST(BrightnessModel, LeavingReadyDropsTheTarget)
{
    Model m;
    m.line("state ready 80 100");
    m.set(0.3);
    EXPECT_EQ(m.line("state not-responding"), VELA_BRIGHTNESS_LINE_CHANGED);
    EXPECT_FALSE(m.model.pending);
    // Locked, still showing what the monitor last had.
    EXPECT_NE(m.json().find(R"("state":"not-responding","value":0.8,)"), std::string::npos);
    EXPECT_EQ(m.set(0.5), "(no)");
    // Recovery by itself.
    m.line("state ready 60 100");
    EXPECT_EQ(m.set(0.5), "set 2 50\n");

    m.line("state no-answer");
    EXPECT_NE(m.json().find(R"("state":"no-answer","value":null,)"), std::string::npos);
}

TEST(BrightnessModel, ANewMaximumKeepsTheTargetInRange)
{
    Model m;
    m.line("state ready 80 100");
    m.set(1.0);
    m.line("state ready 40 50");
    EXPECT_EQ(m.model.max, 50u);
    EXPECT_EQ(m.model.target, 50u);
    EXPECT_TRUE(m.model.pending);
}

TEST(BrightnessModel, OffIsShownAndNotSettable)
{
    Model m;
    m.line("state ready 80 100");
    m.model.off = true;
    EXPECT_NE(m.json().find(R"("state":"off")"), std::string::npos);
    EXPECT_EQ(m.set(0.5), "(no)");
    m.model.off = false;
    EXPECT_EQ(m.set(0.5), "set 1 50\n");
}

// After a helper restarts, nothing the old one said is taken for an answer:
// the sequence keeps growing.
TEST(BrightnessModel, ARestartProbesAgainAndKeepsTheSequence)
{
    Model m;
    m.line("state ready 80 100");
    m.set(0.5);
    vela_brightness_model_restart(&m.model);
    EXPECT_EQ(m.model.state, VELA_BRIGHTNESS_PROBING);
    EXPECT_FALSE(m.model.pending);
    EXPECT_EQ(m.model.seq, 1u);
    m.line("state ready 80 100");
    EXPECT_EQ(m.line("value 1 50"), VELA_BRIGHTNESS_LINE_CHANGED); // the live value, not an answer
    EXPECT_EQ(m.set(0.6), "set 2 60\n");
}

TEST(BrightnessModel, FailLocksTheSlider)
{
    Model m;
    m.line("state ready 80 100");
    m.set(0.2);
    EXPECT_TRUE(vela_brightness_model_fail(&m.model));
    EXPECT_FALSE(m.model.pending);
    EXPECT_NE(m.json().find(R"("state":"not-responding","value":0.8,)"), std::string::npos);
    EXPECT_FALSE(vela_brightness_model_fail(&m.model));
}

TEST(BrightnessModel, BusyAndIdleAreForTheWatchdog)
{
    Model m;
    EXPECT_EQ(m.line("busy"), VELA_BRIGHTNESS_LINE_BUSY);
    EXPECT_EQ(m.line("idle"), VELA_BRIGHTNESS_LINE_IDLE);
}

TEST(BrightnessModel, BadLinesChangeNothing)
{
    Model m;
    for (const char* bad : { "", "state", "state ready", "state ready 80", "state ready 80 0",
             "state ready 80 100 1", "state ready -1 100", "state ready 80 4294967296", "state sleeping", "value",
             "value 1", "value x 1", "value 1 4294967296", "refused 1", "busy now", "set 1 50" }) {
        EXPECT_EQ(m.line(bad), VELA_BRIGHTNESS_LINE_INVALID) << bad;
    }
    EXPECT_EQ(m.model.state, VELA_BRIGHTNESS_PROBING);
    // Before the monitor answered, a value has nothing to update.
    EXPECT_EQ(m.line("value 0 50"), VELA_BRIGHTNESS_LINE_UNCHANGED);
    EXPECT_FALSE(m.model.known);
}

// A laptop's backlight can have more than 16 bits of levels.
TEST(BrightnessModel, LargeMaximums)
{
    Model m;
    m.line("state ready 48000 96000");
    EXPECT_EQ(m.set(0.75), "set 1 72000\n");
    EXPECT_EQ(m.line("value 1 72000"), VELA_BRIGHTNESS_LINE_CHANGED);
    EXPECT_NE(m.json().find(R"("value":0.75,"pending":false,"refused":false,"max":96000,)"), std::string::npos);
}

TEST(BrightnessModel, NamesAreEscaped)
{
    Model m;
    EXPECT_EQ(m.json("A\"B").rfind(R"({"name":"A\"B",)", 0), 0u);
}

} // namespace
