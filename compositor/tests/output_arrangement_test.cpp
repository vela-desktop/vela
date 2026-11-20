// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include <gtest/gtest.h>

extern "C" {
#include "output_arrangement.h"
}

TEST(OutputArrangement, ClosesMiddleGapAndKeepsSavedLayoutOnReconnect)
{
    const vela_output_rect left = { 0, 0, 2560, 1440 };
    const vela_output_rect middle = { 2560, 0, 1920, 1080 };
    const vela_output_rect right = { 4480, 0, 1920, 1200 };
    vela_output_rect unplugged[] = { left, right };
    bool unpluggedPlaced[2] = {};
    vela_output_arrange_rects(unplugged, unpluggedPlaced, 2);
    EXPECT_EQ(unplugged[0].x, 0);
    EXPECT_EQ(unplugged[1].x, 2560);
    EXPECT_EQ(unplugged[1].y, 0);

    vela_output_rect reconnected[] = { left, right, middle };
    bool reconnectedPlaced[3] = {};
    vela_output_arrange_rects(reconnected, reconnectedPlaced, 3);
    EXPECT_EQ(reconnected[1].x, 4480);
    EXPECT_EQ(reconnected[2].x, 2560);
}

TEST(OutputArrangement, ClosesAGapWithoutBreakingTheRightHandGroup)
{
    vela_output_rect rects[] = {
        { 0, 0, 1920, 1080 },
        { 3840, 0, 1920, 1080 },
        { 3840, 1080, 1920, 1080 },
    };
    bool placed[3] = {};
    vela_output_arrange_rects(rects, placed, 3);
    EXPECT_EQ(rects[1].x, 1920);
    EXPECT_EQ(rects[1].y, 0);
    EXPECT_EQ(rects[2].x, rects[1].x);
    EXPECT_EQ(rects[2].y, 1080);
}

TEST(OutputArrangement, ResolvesOverlappedSavedMonitorsAroundTheValidPair)
{
    vela_output_rect rects[] = {
        { 0, 0, 2560, 1440 },
        { 0, 0, 1920, 1200 },
        { 2560, 0, 1920, 1080 },
    };
    bool placed[3] = {};
    vela_output_arrange_rects(rects, placed, 3);
    EXPECT_EQ(rects[0].x, 0);
    EXPECT_EQ(rects[2].x, 2560);
    EXPECT_EQ(rects[2].y, 0);
    EXPECT_FALSE(vela_output_rects_overlap(rects[0], rects[1]));
    EXPECT_FALSE(vela_output_rects_overlap(rects[1], rects[2]));
}

TEST(OutputArrangement, MovesCornerOnlyContactToASharedEdge)
{
    vela_output_rect rects[] = { { 0, 0, 100, 100 }, { 100, 100, 100, 100 } };
    bool placed[2] = {};
    vela_output_arrange_rects(rects, placed, 2);
    EXPECT_EQ(rects[1].x, 100);
    EXPECT_EQ(rects[1].y, 99);
    EXPECT_FALSE(vela_output_rects_overlap(rects[0], rects[1]));
}

TEST(OutputArrangement, SlidesAlongAnEdgeWhenTheProjectionIsBlocked)
{
    vela_output_rect rects[] = { { 0, 0, 100, 100 }, { 100, 0, 100, 50 }, { 80, 40, 20, 20 } };
    bool placed[3] = {};
    vela_output_arrange_rects(rects, placed, 3);
    EXPECT_EQ(rects[1].x, 100);
    EXPECT_EQ(rects[1].y, 0);
    EXPECT_EQ(rects[2].x, 100);
    EXPECT_EQ(rects[2].y, 50);
}
