// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Animation curves and tweens (motion.h): the same as CSS and QML cubic-bezier
// curves, which the shell uses too.

extern "C" {
#include "motion.h"
}

#include <gtest/gtest.h>

#include <cmath>

namespace {

// The curve's value at x, computed another way: sampling the parameter t very
// densely and taking the point with the closest x.
double reference(double x1, double y1, double x2, double y2, double x)
{
    auto bezier = [](double a, double b, double t) {
        const double u = 1.0 - t;
        return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
    };
    double best = 0.0;
    double distance = 2.0;
    for (int i = 0; i <= 200000; ++i) {
        const double t = i / 200000.0;
        const double d = std::abs(bezier(x1, x2, t) - x);
        if (d < distance) {
            distance = d;
            best = bezier(y1, y2, t);
        }
    }
    return best;
}

} // namespace

TEST(Motion, EndpointsAndClamping)
{
    const vela_curve* curve = &vela_decelerate;
    EXPECT_EQ(vela_curve_eval(curve, 0.0), 0.0);
    EXPECT_EQ(vela_curve_eval(curve, 1.0), 1.0);
    EXPECT_EQ(vela_curve_eval(curve, -0.5), 0.0);
    EXPECT_EQ(vela_curve_eval(curve, 1.5), 1.0);
}

TEST(Motion, LinearCurveIsIdentity)
{
    const vela_curve linear { 0.0, 0.0, 1.0, 1.0 };
    for (double x = 0.0; x <= 1.0; x += 0.05) {
        EXPECT_NEAR(vela_curve_eval(&linear, x), x, 1e-6);
    }
}

TEST(Motion, MatchesCubicBezierDefinition)
{
    struct Case {
        double x1, y1, x2, y2;
    };
    for (const Case& c : { Case { 0.0, 0.0, 0.2, 1.0 }, Case { 0.25, 0.1, 0.25, 1.0 }, Case { 0.42, 0.0, 0.58, 1.0 },
             Case { 0.1, 0.9, 0.2, 1.0 } }) {
        const vela_curve curve { c.x1, c.y1, c.x2, c.y2 };
        for (double x = 0.05; x < 1.0; x += 0.05) {
            EXPECT_NEAR(vela_curve_eval(&curve, x), reference(c.x1, c.y1, c.x2, c.y2, x), 1e-4) << "x=" << x;
        }
    }
}

TEST(Motion, DecelerateIsMonotonicAndFrontLoaded)
{
    const vela_curve* curve = &vela_decelerate;
    double previous = 0.0;
    for (int i = 1; i <= 1000; ++i) {
        const double value = vela_curve_eval(curve, i / 1000.0);
        EXPECT_GE(value, previous);
        previous = value;
    }
    // A decisive start: halfway through the time it's well over halfway, but
    // not so much that it vanishes in the first frames at high refresh rates.
    EXPECT_GT(vela_curve_eval(curve, 0.5), 0.7);
    EXPECT_LT(vela_curve_eval(curve, 3.0 / 45.0), 0.35); // 3 frames at 180 Hz of a 250 ms animation
}

TEST(Motion, TweenStartsAtFirstRead)
{
    vela_tween tween;
    vela_tween_start(&tween, "test", 250.0, &vela_decelerate);
    EXPECT_FALSE(vela_tween_finished(&tween, 1000.0)); // not read yet: it hasn't started
    EXPECT_EQ(vela_tween_progress(&tween, 1000.0), 0.0); // the first frame is the initial state
    EXPECT_GT(vela_tween_progress(&tween, 1100.0), 0.0);
    EXPECT_FALSE(vela_tween_finished(&tween, 1249.0));
    EXPECT_TRUE(vela_tween_finished(&tween, 1250.0));
    EXPECT_EQ(vela_tween_progress(&tween, 1300.0), 1.0);
}

TEST(Motion, TweenWithoutCurveIsLinear)
{
    vela_tween tween;
    vela_tween_start(&tween, "test", 100.0, nullptr);
    EXPECT_EQ(vela_tween_progress(&tween, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(vela_tween_progress(&tween, 25.0), 0.25);
    EXPECT_DOUBLE_EQ(vela_tween_progress(&tween, 100.0), 1.0);
}

// Two outputs read the same tween at their own presentation times, in
// wall-clock order (docs/visual-coherence.md §6.2): the state may go back from
// one to the next, the end waits for the wall clock.
TEST(Motion, TweenSampledByOutputsRetiresOnTheWallClock)
{
    vela_tween tween;
    vela_tween_start(&tween, "test", 100.0, nullptr);
    EXPECT_EQ(vela_tween_progress(&tween, 22.0), 0.0); // the first read fixes the start
    EXPECT_EQ(vela_tween_progress(&tween, 11.0), 0.0); // an earlier frame: the initial state, not a jump back
    EXPECT_DOUBLE_EQ(vela_tween_progress(&tween, 72.0), 0.5);
    EXPECT_DOUBLE_EQ(vela_tween_progress(&tween, 61.0), 0.39); // the other output, its own time
    EXPECT_EQ(vela_tween_progress(&tween, 130.0), 1.0);
    EXPECT_FALSE(vela_tween_retired(&tween, 121.0)); // a frame presented at 125 is still to come
    EXPECT_TRUE(vela_tween_retired(&tween, 122.0));
    EXPECT_TRUE(vela_tween_retired(&tween, 123.0));
}

TEST(Motion, TweenNotReadIsNotRetired)
{
    vela_tween tween;
    vela_tween_start(&tween, "test", 100.0, nullptr);
    EXPECT_FALSE(vela_tween_retired(&tween, 1e9)); // no frame has shown it yet
}
