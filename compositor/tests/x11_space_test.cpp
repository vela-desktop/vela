// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The X11 root in output pixels (x11_space.h, docs/renderer.md §3.9): where
// each output goes, the conversions to and from the logical layout, Xft.dpi.

extern "C" {
#include "x11_space.h"
}

#include <gtest/gtest.h>

#include <vector>

namespace {

vela_x11_output output(int x, int y, int pixelWidth, int pixelHeight, double scale)
{
    vela_x11_output o {};
    o.x = x;
    o.y = y;
    o.width = (int)(pixelWidth / scale + 0.5);
    o.height = (int)(pixelHeight / scale + 0.5);
    o.scale = scale;
    o.pixel_width = pixelWidth;
    o.pixel_height = pixelHeight;
    return o;
}

bool overlap(const vela_x11_output& a, const vela_x11_output& b)
{
    return a.root_x < b.root_x + b.pixel_width && b.root_x < a.root_x + a.pixel_width
        && a.root_y < b.root_y + b.pixel_height && b.root_y < a.root_y + a.pixel_height;
}

} // namespace

TEST(X11Space, OneScaledOutputIsItsPixels)
{
    vela_x11_output o = output(0, 0, 2560, 1440, 1.25);
    vela_x11_place(&o, 1);
    EXPECT_EQ(o.root_x, 0);
    EXPECT_EQ(o.root_y, 0);
    double x = 0, y = 0;
    vela_x11_from_logical(&o, 2048, 1152, &x, &y);
    EXPECT_DOUBLE_EQ(x, 2560);
    EXPECT_DOUBLE_EQ(y, 1440);
    double lx = 0, ly = 0;
    vela_x11_to_logical(&o, 1280, 720, &lx, &ly);
    EXPECT_DOUBLE_EQ(lx, 1024);
    EXPECT_DOUBLE_EQ(ly, 576);
}

// The user's desk: a 1080p monitor at 100% above a 1440p one at 125%.
TEST(X11Space, MixedScalesStackedVertically)
{
    std::vector<vela_x11_output> o { output(0, 0, 1920, 1080, 1.0), output(0, 1080, 2560, 1440, 1.25) };
    vela_x11_place(o.data(), o.size());
    EXPECT_EQ(o[0].root_x, 0);
    EXPECT_EQ(o[0].root_y, 0);
    EXPECT_EQ(o[1].root_x, 0);
    EXPECT_EQ(o[1].root_y, 1350);
    EXPECT_FALSE(overlap(o[0], o[1]));
    // A point in each output's middle comes back where it was.
    for (const vela_x11_output& out : o) {
        double x = 0, y = 0, lx = 0, ly = 0;
        vela_x11_from_logical(&out, out.x + out.width / 2.0, out.y + out.height / 2.0, &x, &y);
        EXPECT_EQ(vela_x11_output_at(o.data(), o.size(), x, y), &out);
        vela_x11_to_logical(&out, x, y, &lx, &ly);
        EXPECT_DOUBLE_EQ(lx, out.x + out.width / 2.0);
        EXPECT_DOUBLE_EQ(ly, out.y + out.height / 2.0);
    }
}

TEST(X11Space, SideBySideNeverOverlap)
{
    for (double scale : { 1.0, 1.25, 1.5, 1.75, 2.0, 2.25 }) {
        // At 150% 2560 pixels are 1706.67 units, rounded to 1707.
        std::vector<vela_x11_output> o { output(0, 0, 2560, 1440, scale) };
        o.push_back(output(o[0].width, 0, 1920, 1080, 1.0));
        o.push_back(output(o[1].x + o[1].width, 0, 3840, 2160, scale));
        vela_x11_place(o.data(), o.size());
        EXPECT_FALSE(overlap(o[0], o[1])) << scale;
        EXPECT_FALSE(overlap(o[1], o[2])) << scale;
        EXPECT_FALSE(overlap(o[0], o[2])) << scale;
        // The left one touches the next: no gap after the densest.
        EXPECT_LE(o[1].root_x - (o[0].root_x + o[0].pixel_width), 1) << scale;
    }
}

TEST(X11Space, RootStartsAtZero)
{
    std::vector<vela_x11_output> o { output(-1920, -200, 1920, 1080, 1.0), output(0, 0, 2560, 1440, 1.0) };
    vela_x11_place(o.data(), o.size());
    EXPECT_EQ(o[0].root_x, 0);
    EXPECT_EQ(o[0].root_y, 0);
    EXPECT_EQ(o[1].root_x, 1920);
    EXPECT_EQ(o[1].root_y, 200);
}

TEST(X11Space, NearestOutputOutsideAll)
{
    std::vector<vela_x11_output> o { output(0, 0, 1920, 1080, 1.0), output(0, 1080, 2560, 1440, 1.25) };
    vela_x11_place(o.data(), o.size());
    // In the gap right of the upper output, and below everything.
    EXPECT_EQ(vela_x11_output_at(o.data(), o.size(), 2200, 100), &o[0]);
    EXPECT_EQ(vela_x11_output_at(o.data(), o.size(), 100, 5000), &o[1]);
    EXPECT_EQ(vela_x11_output_at(o.data(), 0, 0, 0), nullptr);
}

TEST(X11Space, DpiFollowsTheLargestScale)
{
    std::vector<vela_x11_output> o { output(0, 0, 1920, 1080, 1.0) };
    EXPECT_EQ(vela_x11_dpi(o.data(), o.size()), 96);
    o.push_back(output(1920, 0, 2560, 1440, 1.25));
    EXPECT_EQ(vela_x11_dpi(o.data(), o.size()), 120);
    o.push_back(output(0, 1080, 3840, 2160, 1.75));
    EXPECT_EQ(vela_x11_dpi(o.data(), o.size()), 168);
    EXPECT_EQ(vela_x11_dpi(nullptr, 0), 96);
}
