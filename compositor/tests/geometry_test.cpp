// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The geometry of sharpness (geometry.h, docs/renderer.md §3): scale from DPI,
// VELA_SCALE, exact-pixel placement of snapped and maximized windows, edges in
// pixels, the 1:1 copy.

extern "C" {
#include "geometry.h"
}

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>

namespace {

// Physical width and height (mm) of an `inches` inch screen.
void physicalSize(double inches, int width, int height, double& widthMm, double& heightMm)
{
    const double diagonal = std::hypot(width, height);
    widthMm = inches * 25.4 * width / diagonal;
    heightMm = inches * 25.4 * height / diagonal;
}

} // namespace

// The table of §3.8.
TEST(Geometry, DefaultScaleFromDpi)
{
    struct Case {
        const char* name;
        double inches;
        int width, height;
        float scale;
    };
    for (const Case& c : {
             Case { "DP-1", 24, 1920, 1080, 1.0f },
             Case { "DP-1", 27, 2560, 1440, 1.25f },
             Case { "DP-1", 32, 2560, 1440, 1.0f },
             Case { "DP-1", 34, 3440, 1440, 1.25f },
             Case { "DP-1", 27, 3840, 2160, 1.75f },
             Case { "HDMI-A-1", 32, 3840, 2160, 1.5f },
             Case { "eDP-1", 15.6, 1920, 1080, 1.25f },
             Case { "eDP-1", 14, 1920, 1200, 1.5f },
             Case { "eDP-1", 16, 2560, 1600, 1.75f },
             Case { "eDP-1", 14, 2880, 1800, 2.25f },
         }) {
        double widthMm = 0, heightMm = 0, dpi = 0;
        physicalSize(c.inches, c.width, c.height, widthMm, heightMm);
        EXPECT_EQ(vela_scale_for_dpi(widthMm, heightMm, c.width, c.height, vela_is_internal_panel(c.name), &dpi), c.scale)
            << c.inches << "\" " << c.width << "x" << c.height;
        EXPECT_GT(dpi, 80.0);
    }
}

TEST(Geometry, AbsurdPhysicalSizesFallBackTo100Percent)
{
    double dpi = -1;
    EXPECT_EQ(vela_scale_for_dpi(0, 0, 1920, 1080, false, &dpi), 1.0f); // projector: no size
    EXPECT_EQ(dpi, 0.0);
    double w = 0, h = 0;
    physicalSize(150, 3840, 2160, w, h); // 150": a TV or a made-up EDID
    EXPECT_EQ(vela_scale_for_dpi(w, h, 3840, 2160, false, &dpi), 1.0f);
    EXPECT_EQ(vela_scale_for_dpi(160, 90, 3840, 2160, false, &dpi), 1.0f); // 7": too small
    EXPECT_EQ(vela_scale_for_dpi(600, 600, 3840, 2160, false, &dpi), 1.0f); // wrong proportions
    EXPECT_EQ(vela_scale_for_dpi(600, 340, 0, 0, false, &dpi), 1.0f);
}

TEST(Geometry, ScaleIsClampedAndInQuarterSteps)
{
    double w = 0, h = 0, dpi = 0;
    physicalSize(10, 7680, 4320, w, h); // 881 DPI
    EXPECT_EQ(vela_scale_for_dpi(w, h, 7680, 4320, false, &dpi), 3.0f);
    for (double inches = 11; inches < 60; inches += 0.7) {
        physicalSize(inches, 3840, 2160, w, h);
        const float scale = vela_scale_for_dpi(w, h, 3840, 2160, false, &dpi);
        EXPECT_EQ(scale * 4.0f, std::round(scale * 4.0f));
        EXPECT_GE(scale, 1.0f);
        EXPECT_LE(scale, 3.0f);
    }
}

TEST(Geometry, RequestedScale)
{
    EXPECT_EQ(vela_requested_scale(nullptr, "DP-1"), 0.0f);
    EXPECT_EQ(vela_requested_scale("", "DP-1"), 0.0f);
    EXPECT_EQ(vela_requested_scale("1.25", "DP-1"), 1.25f);
    EXPECT_EQ(vela_requested_scale("DP-1=1.5,HDMI-A-1=1", "DP-1"), 1.5f);
    EXPECT_EQ(vela_requested_scale("DP-1=1.5,HDMI-A-1=1", "HDMI-A-1"), 1.0f);
    EXPECT_EQ(vela_requested_scale("DP-1=1.5,HDMI-A-1=1", "DP-2"), 0.0f);
    EXPECT_EQ(vela_requested_scale("DP-10=2,DP-1=1.75", "DP-1"), 1.75f); // names starting the same way
}

TEST(Geometry, BufferPixelsLikeFractionalScale)
{
    EXPECT_EQ(vela_buffer_pixels(853, 1.5), 1280); // half of 2560 at 150%
    EXPECT_EQ(vela_buffer_pixels(1707, 1.5), 2561); // 2560 isn't possible: one extra pixel
    EXPECT_EQ(vela_buffer_pixels(1706, 1.5), 2559);
    EXPECT_EQ(vela_buffer_pixels(1024, 1.25), 1280);
    EXPECT_EQ(vela_buffer_pixels(100, 1.0), 100);
}

// For every scale, output and split: the buffer covers exactly the area's
// pixels; when that's impossible, the extra pixel goes only past a free edge
// of the output; otherwise it stays inside.
TEST(Geometry, PlacementCoversThePixelsExactly)
{
    const double scales[] = { 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0 };
    const int screens[] = { 1280, 1366, 1440, 1920, 2560, 2880, 3440, 3840 };
    // Splits in twelfths, like the snap layouts: halves, thirds, quarters,
    // 2/3.
    const int splits[][2] = { { 0, 12 }, { 0, 6 }, { 6, 12 }, { 0, 4 }, { 4, 8 }, { 8, 12 }, { 0, 3 }, { 3, 9 },
        { 9, 12 }, { 0, 8 }, { 8, 12 } };
    int exactCount = 0;
    int total = 0;
    for (double scale : scales) {
        for (int screen : screens) {
            for (const auto& split : splits) {
                const int start = screen * split[0] / 12;
                const int end = screen * split[1] / 12;
                const int size = end - start;
                for (bool open : { true, false }) {
                    const vela_axis axis = vela_place_axis(start, size, screen, scale, open, open);
                    const int64_t pixels = vela_buffer_pixels(axis.size, scale);
                    const double first = axis.offset * scale;
                    ++total;
                    if (pixels == size) {
                        ++exactCount;
                        EXPECT_DOUBLE_EQ(first, start) << scale << " " << screen << " " << split[0];
                        continue;
                    }
                    const bool touches = start == 0 || end == screen;
                    if (open && touches) {
                        // It covers the area and spills only off the output.
                        EXPECT_GT(pixels, size);
                        // (first is start / scale × scale: up to rounding)
                        EXPECT_LE(first, start + 1e-6);
                        EXPECT_GE(first + pixels, end - 1e-6);
                        EXPECT_TRUE(first < -1e-6 || first + pixels > screen + 1e-6);
                        EXPECT_LE(pixels - size, 2);
                    } else {
                        // Inside the area, at most one pixel smaller.
                        EXPECT_LT(pixels, size);
                        EXPECT_GE(pixels, size - 2);
                        EXPECT_DOUBLE_EQ(first, start);
                    }
                }
            }
        }
    }
    // Most cases have an exact size (the test is meaningful).
    EXPECT_GT(exactCount, total / 2);
}

// The case of §3.5: at 150%, 2560 pixels would be 1706.67 units.
TEST(Geometry, MaximizedAt150PercentSpillsOnePixelOffScreen)
{
    const vela_axis open = vela_place_axis(0, 2560, 2560, 1.5, true, true);
    EXPECT_EQ(open.size, 1707);
    EXPECT_EQ(open.offset, 0.0);
    EXPECT_EQ(vela_buffer_pixels(open.size, 1.5), 2561);
    // With an output to the right, the extra pixel goes left.
    const vela_axis rightNeighbour = vela_place_axis(0, 2560, 2560, 1.5, true, false);
    EXPECT_EQ(rightNeighbour.size, 1707);
    EXPECT_DOUBLE_EQ(rightNeighbour.offset * 1.5, -1.0);
    // With outputs on both sides it stays inside.
    const vela_axis closed = vela_place_axis(0, 2560, 2560, 1.5, false, false);
    EXPECT_EQ(closed.size, 1706);
}

TEST(Geometry, SnappedHalvesNeverGapOrOverlap)
{
    for (double scale : { 1.0, 1.25, 1.5, 1.75, 2.0, 2.25 }) {
        for (int screen : { 1366, 1920, 2560, 3440, 3840 }) {
            const int middle = screen / 2;
            const vela_axis left = vela_place_axis(0, middle, screen, scale, true, true);
            const vela_axis right = vela_place_axis(middle, screen - middle, screen, scale, true, true);
            const int64_t leftEnd = int64_t(std::lround(left.offset * scale)) + vela_buffer_pixels(left.size, scale);
            const int64_t rightStart = int64_t(std::lround(right.offset * scale));
            EXPECT_LE(leftEnd, rightStart + 1) << scale << " " << screen; // no visible overlap
            EXPECT_GE(leftEnd, rightStart - 1) << scale << " " << screen; // no visible gap
        }
    }
}

// Edges are rounded, not position and size (§3.2): adjacent rectangles stay
// adjacent at every scale and position.
TEST(Geometry, AdjacentRectanglesStayAdjacent)
{
    for (double scale : { 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 3.0 }) {
        for (double x = 0.0; x < 40.0; x += 0.37) {
            const double width = 13.3;
            const vela_pixel_box a = vela_edges_to_pixels(x, 0, width, 10, 0, 0, scale);
            const vela_pixel_box b = vela_edges_to_pixels(x + width, 0, width, 10, 0, 0, scale);
            EXPECT_EQ(a.x + a.width, b.x) << scale << " " << x;
        }
    }
    const vela_pixel_box box = vela_edges_to_pixels(110, 50, 100, 40, 100, 50, 1.5);
    EXPECT_EQ(box.x, 15);
    EXPECT_EQ(box.y, 0);
    EXPECT_EQ(box.width, 150);
    EXPECT_EQ(box.height, 60);
}

// A rectangle moved by whole pixels moves its pixels by as many and keeps its
// size, also across the output's left and top edges, where its coordinates
// turn negative: a window dragged partly off the output (its title's image
// sits there, decoration.c). lround would round -1.5 to -2 and 1.5 to 2.
TEST(Geometry, WholePixelMovesAcrossTheOutputEdge)
{
    long moved_wrong = 0;
    long resized = 0;
    long checked = 0;
    for (double scale : { 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 3.0 }) {
        const int units = vela_whole_pixel_units(static_cast<float>(scale));
        ASSERT_GT(units, 0) << scale;
        const int step = static_cast<int>(std::lround(units * scale));
        for (int origin : { 0, 1920 }) {
            for (int x = origin - 300; x < origin + 300; ++x) {
                for (double width : { 1.0, 10.5, 37.0 }) {
                    const vela_pixel_box a = vela_edges_to_pixels(x, x, width, width, origin, origin, scale);
                    const vela_pixel_box b =
                        vela_edges_to_pixels(x + units, x + units, width, width, origin, origin, scale);
                    moved_wrong += b.x != a.x + step || b.y != a.y + step;
                    resized += b.width != a.width || b.height != a.height;
                    ++checked;
                }
            }
        }
    }
    EXPECT_EQ(moved_wrong, 0) << "of " << checked;
    EXPECT_EQ(resized, 0) << "of " << checked;
    // Halves round up on both sides of the edge.
    EXPECT_EQ(vela_edges_to_pixels(-1, 0, 2, 1, 0, 0, 1.5).x, -1);
    EXPECT_EQ(vela_edges_to_pixels(-1, 0, 2, 1, 0, 0, 1.5).width, 3);
    EXPECT_EQ(vela_edges_to_pixels(1, 0, 2, 1, 0, 0, 1.5).x, 2);
}

TEST(Geometry, BufferMatchesArea)
{
    EXPECT_TRUE(vela_buffer_matches_area(0, 0, 1280, 720, 1024, 576, 1.25)); // drawn at the output scale
    EXPECT_TRUE(vela_buffer_matches_area(0, 0, 1280, 721, 853.5, 480.5, 1.5)); // within rounding
    EXPECT_FALSE(vela_buffer_matches_area(0, 0, 1024, 576, 1024, 576, 1.25)); // integer scale: must be magnified
    EXPECT_FALSE(vela_buffer_matches_area(0.5, 0, 1280, 720, 1024, 576, 1.25)); // non-integer source (viewport)
}

TEST(Geometry, OneToOneCopy)
{
    EXPECT_TRUE(vela_one_to_one(0, 0, 1280, 720, 1280, 720, false));
    EXPECT_TRUE(vela_one_to_one(10, 20, 720, 1280, 1280, 720, true)); // rotated by 90°
    EXPECT_FALSE(vela_one_to_one(0, 0, 1280, 720, 1281, 720, false));
    EXPECT_FALSE(vela_one_to_one(0.5, 0, 1280, 720, 1280, 720, false));
}

TEST(Geometry, PhysicalEdges)
{
    // A free area touching the edges stays exactly on the output's pixels.
    EXPECT_EQ(vela_physical_edge(0, 0, 2048, 2560, 1.25), 0);
    EXPECT_EQ(vela_physical_edge(2048, 0, 2048, 2560, 1.25), 2560);
    EXPECT_EQ(vela_physical_edge(-5, 0, 2048, 2560, 1.25), 0);
    EXPECT_EQ(vela_physical_edge(1104, 0, 1152, 1440, 1.25), 1380); // above the 48-high taskbar
    EXPECT_EQ(vela_physical_edge(100, 100, 2148, 2560, 1.25), 0); // output not at the origin
}

// The title's ink margin (text.h, decoration.c): the ink needs some pixels
// left of the line box; the image takes the whole number of
// vela_whole_pixel_units steps that holds them and moves left by those units,
// so the line box lands on the pixels it had without a margin, at every
// position of the window. Taking just the pixels needed and moving by their
// exact fraction (pixels / scale) would land the line box a pixel off at some
// positions: the negative control.
TEST(Geometry, WholePixelUnitsKeepTheLineBoxOnItsPixels)
{
    EXPECT_EQ(vela_whole_pixel_units(1.0f), 1);
    EXPECT_EQ(vela_whole_pixel_units(1.25f), 4);
    EXPECT_EQ(vela_whole_pixel_units(1.5f), 2);
    EXPECT_EQ(vela_whole_pixel_units(1.75f), 4);
    EXPECT_EQ(vela_whole_pixel_units(2.0f), 1);
    EXPECT_EQ(vela_whole_pixel_units(2.25f), 4);
    EXPECT_EQ(vela_whole_pixel_units(1.125f), 8);
    EXPECT_EQ(vela_whole_pixel_units(1.1f), 0); // not a short binary fraction: no exact halves to round either way
    EXPECT_EQ(vela_whole_pixel_units(0.0f), 0);

    for (float scale : { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.25f, 2.5f, 3.0f }) {
        const int units = vela_whole_pixel_units(scale);
        ASSERT_GT(units, 0) << scale;
        const int step = static_cast<int>(std::lround(units * static_cast<double>(scale))); // pixels
        long whole_moved = 0;
        long fraction_moved = 0;
        long positions = 0;
        // Windows at every whole position on an output at 0 and on one to its
        // right, also partly off the output's left edge (the image's left
        // edge on one side of it, the line box on the other); the title after
        // the margin or after the icon (decoration.c); ink 1 to 6 pixels left
        // of the line box.
        for (int origin : { 0, 1920 }) {
            for (int window = origin - 200; window < origin + 2400; ++window) {
                for (int title : { 12, 36 }) {
                    for (int needed = 1; needed <= 6; ++needed) {
                        const int line = vela_edges_to_pixels(window + title, 0, 10, 10, origin, 0, scale).x;
                        const int steps = (needed + step - 1) / step;
                        const int whole =
                            vela_edges_to_pixels(window + (title - steps * units), 0, 10, 10, origin, 0, scale).x
                            + steps * step;
                        const int fraction =
                            vela_edges_to_pixels(
                                window + (title - needed / static_cast<double>(scale)), 0, 10, 10, origin, 0, scale)
                                .x
                            + needed;
                        whole_moved += whole != line;
                        fraction_moved += fraction != line;
                        ++positions;
                    }
                }
            }
        }
        EXPECT_EQ(whole_moved, 0) << scale << ": " << whole_moved << " of " << positions;
        if (units > 1) {
            // The fraction rounds some positions the other way.
            EXPECT_GT(fraction_moved, positions / 100) << scale << ": " << fraction_moved << " of " << positions;
        } else {
            EXPECT_EQ(fraction_moved, 0) << scale; // whole pixels per unit: nothing to round
        }
        std::printf("scale %g: line box moved at %ld of %ld positions with whole units, %ld with the fraction\n",
            static_cast<double>(scale), whole_moved, positions, fraction_moved);
    }
}
