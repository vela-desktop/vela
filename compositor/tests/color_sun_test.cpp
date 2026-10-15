// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Night light, color filters and sun times (color.h, sun.h).

extern "C" {
#include "color.h"
#include "scene/night_light_policy.h"
#include "sun.h"
}

#include <gtest/gtest.h>

TEST(Color, NightKelvinFromStrength)
{
    EXPECT_EQ(vela_night_kelvin(0), 6500.0);
    EXPECT_EQ(vela_night_kelvin(100), 1700.0);
    EXPECT_EQ(vela_night_kelvin(50), 4100.0);
    EXPECT_EQ(vela_night_kelvin(-10), 6500.0);
    EXPECT_EQ(vela_night_kelvin(500), 1700.0);
}

TEST(Color, NightGainsAreNeutralAt6500AndWarmBelow)
{
    float gains[3];
    vela_night_gains(6500.0, gains);
    for (float g : gains) {
        EXPECT_NEAR(g, 1.0f, 1e-3f);
    }
    float previousBlue = 1.0f;
    for (double kelvin = 6000.0; kelvin >= 1700.0; kelvin -= 500.0) {
        vela_night_gains(kelvin, gains);
        EXPECT_NEAR(gains[0], 1.0f, 1e-3f) << kelvin; // red stays
        EXPECT_LE(gains[1], 1.0f);
        EXPECT_LT(gains[2], previousBlue) << kelvin; // blue always drops
        previousBlue = gains[2];
    }
}

// Every filter keeps white white (rows summing to 1): it changes colors, not
// the screen's brightness.
TEST(Color, FiltersKeepWhiteWhite)
{
    for (const char* kind : { "grayscale", "deuteranopia", "protanopia", "tritanopia" }) {
        float m[9];
        vela_filter_matrix(kind, m);
        for (int row = 0; row < 3; ++row) {
            EXPECT_NEAR(m[row * 3] + m[row * 3 + 1] + m[row * 3 + 2], 1.0f, 1e-3f) << kind << " riga " << row;
        }
    }
    float gray[9];
    vela_filter_matrix("grayscale", gray);
    for (int row = 1; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            EXPECT_EQ(gray[row * 3 + column], gray[column]); // three equal channels
        }
    }
}

TEST(Color, MatrixMultiplyWithIdentity)
{
    float m[9];
    vela_filter_matrix("protanopia", m);
    float left[9];
    float right[9];
    vela_matrix_multiply(vela_identity, m, left);
    vela_matrix_multiply(m, vela_identity, right);
    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ(left[i], m[i]) << i;
        EXPECT_EQ(right[i], m[i]) << i;
    }
}

/** Check shader repaint policy on fade completion and backend switches. */
TEST(NightLightBackend, FullRepaintOnShaderFadeAndBackendSwitch)
{
    EXPECT_TRUE(vela_night_light_repaint_required(false, true, true, true, false, false));
    EXPECT_TRUE(vela_night_light_repaint_required(false, true, true, true, false, true));
    EXPECT_TRUE(vela_night_light_repaint_required(false, true, false, false, false, false));
    EXPECT_TRUE(vela_night_light_repaint_required(false, true, true, false, true, false));
    EXPECT_TRUE(vela_night_light_repaint_required(true, false, true, false, false, true));
    EXPECT_FALSE(vela_night_light_repaint_required(false, true, true, false, false, true));
    EXPECT_FALSE(vela_night_light_repaint_required(false, false, true, true, false, false));
}

/** Check that a rejected LUT clear is deferred only until explicit retry. */
TEST(NightLightBackend, ClearDeferralCanBeOverriddenByExplicitRetry)
{
    EXPECT_TRUE(vela_night_light_defer_clear(true, true, false, true));
    EXPECT_FALSE(vela_night_light_defer_clear(true, true, true, true));
    EXPECT_FALSE(vela_night_light_defer_clear(false, true, false, true));
    EXPECT_FALSE(vela_night_light_defer_clear(true, false, false, true));
    EXPECT_FALSE(vela_night_light_defer_clear(true, true, false, false));
}

TEST(Sun, ParseZoneCoordinates)
{
    double latitude = 0, longitude = 0;
    ASSERT_TRUE(vela_sun_parse_coordinates("+4154+01229", &latitude, &longitude)); // Europe/Rome
    EXPECT_NEAR(latitude, 41.9, 1e-9);
    EXPECT_NEAR(longitude, 12.4833333, 1e-6);
    ASSERT_TRUE(vela_sun_parse_coordinates("+404251-0740023", &latitude, &longitude)); // America/New_York
    EXPECT_NEAR(latitude, 40.714166, 1e-5);
    EXPECT_NEAR(longitude, -74.006388, 1e-5);
    ASSERT_TRUE(vela_sun_parse_coordinates("-3352+15113", &latitude, &longitude)); // Australia/Sydney
    EXPECT_LT(latitude, 0.0);
    EXPECT_FALSE(vela_sun_parse_coordinates("", &latitude, &longitude));
    EXPECT_FALSE(vela_sun_parse_coordinates("4154+01229", &latitude, &longitude));
    EXPECT_FALSE(vela_sun_parse_coordinates("+41", &latitude, &longitude));
}

namespace {

tm day(int yday, long offsetSeconds)
{
    tm t {};
    t.tm_yday = yday;
    t.tm_gmtoff = offsetSeconds;
    return t;
}

int minutes(int hours, int mins)
{
    return hours * 60 + mins;
}

} // namespace

// Rome on June 21 (summer time): sunrise around 5:35, sunset around 20:48.
TEST(Sun, RomeSummerSolstice)
{
    const tm june21 = day(171, 2 * 3600);
    EXPECT_NEAR(vela_sun_time(true, &june21, 41.9, 12.48), minutes(5, 35), 6);
    EXPECT_NEAR(vela_sun_time(false, &june21, 41.9, 12.48), minutes(20, 48), 6);
}

// Rome on December 21 (standard time): sunrise around 7:33, sunset around
// 16:42.
TEST(Sun, RomeWinterSolstice)
{
    const tm december21 = day(354, 3600);
    EXPECT_NEAR(vela_sun_time(true, &december21, 41.9, 12.48), minutes(7, 33), 6);
    EXPECT_NEAR(vela_sun_time(false, &december21, 41.9, 12.48), minutes(16, 42), 6);
}

TEST(Sun, PolarDayAndNight)
{
    // Tromsø: in summer the sun doesn't set, in winter it doesn't rise.
    const tm summer = day(171, 2 * 3600);
    const tm winter = day(354, 3600);
    EXPECT_EQ(vela_sun_time(false, &summer, 69.65, 18.96), -1);
    EXPECT_EQ(vela_sun_time(true, &winter, 69.65, 18.96), -1);
}

TEST(Sun, ParseClockAndRanges)
{
    EXPECT_EQ(vela_parse_clock("21:30", -1), minutes(21, 30));
    EXPECT_EQ(vela_parse_clock("7:05", -1), minutes(7, 5));
    EXPECT_EQ(vela_parse_clock("24:00", -1), -1);
    EXPECT_EQ(vela_parse_clock("12:60", -1), -1);
    EXPECT_EQ(vela_parse_clock("sera", 42), 42);
    // From 21 to 7: across midnight.
    EXPECT_TRUE(vela_in_range(minutes(23, 0), minutes(21, 0), minutes(7, 0)));
    EXPECT_TRUE(vela_in_range(minutes(6, 59), minutes(21, 0), minutes(7, 0)));
    EXPECT_FALSE(vela_in_range(minutes(7, 0), minutes(21, 0), minutes(7, 0)));
    EXPECT_FALSE(vela_in_range(minutes(12, 0), minutes(21, 0), minutes(7, 0)));
    // From 13 to 15: the same day.
    EXPECT_TRUE(vela_in_range(minutes(14, 0), minutes(13, 0), minutes(15, 0)));
    EXPECT_FALSE(vela_in_range(minutes(15, 0), minutes(13, 0), minutes(15, 0)));
}
