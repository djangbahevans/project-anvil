#include "anvil/core/smooth_scroll.hpp"

#include <cmath>
#include <gtest/gtest.h>
#include <random>

using anvil::core::ScrollPosition;
using anvil::core::SmoothScroll;

namespace {

TEST(SmoothScroll, StartsAlittleAheadAndEndsAfter125Milliseconds) {
    const SmoothScroll scroll({0, 100}, {0, 400}, 600, 600, 1000);
    EXPECT_EQ(scroll.to(), (ScrollPosition{0, 400}));
    // Ten of 135 milliseconds are done at once.
    EXPECT_EQ(scroll.at(1000).top, 100 + static_cast<int>(300 * (1 - std::pow(125.0 / 135, 3))));
    EXPECT_FALSE(scroll.done(1124));
    EXPECT_LT(scroll.at(1124).top, 400);
    EXPECT_TRUE(scroll.done(1125));
    EXPECT_EQ(scroll.at(1125), (ScrollPosition{0, 400}));
    EXPECT_EQ(scroll.at(5000), (ScrollPosition{0, 400}));
}

TEST(SmoothScroll, EasesOutSoThatMostOfTheWayIsDoneEarly) {
    const SmoothScroll scroll({0, 0}, {1000, 0}, 600, 600, 0);
    // Half way through the 135 milliseconds, seven eighths of the way are done.
    EXPECT_NEAR(scroll.at(58).left, 875, 3);
}

TEST(SmoothScroll, ALongWaySkipsItsMiddle) {
    const SmoothScroll down({0, 0}, {0, 10000}, 600, 400, 0);
    // The first third of the time goes three quarters of a view from the start.
    for (std::int64_t now = 0; now < 34; ++now) {
        EXPECT_LE(down.at(now).top, 300) << now;
    }
    // The rest ends three quarters of a view away from the target.
    for (std::int64_t now = 35; now < 125; ++now) {
        EXPECT_GE(down.at(now).top, 9700) << now;
    }
    const SmoothScroll up({0, 10000}, {0, 0}, 600, 400, 0);
    EXPECT_GE(up.at(30).top, 9700);
    EXPECT_LE(up.at(60).top, 300);
}

// VS Code's operation, written out as in `scrollable.ts`.
int reference(double from, double to, double view, double completion) {
    const auto ease = [](double a, double b, double t) {
        return a + (b - a) * (1 - std::pow(1 - t, 3));
    };
    if (std::abs(from - to) > 2.5 * view) {
        const double stop1 = from < to ? from + 0.75 * view : from - 0.75 * view;
        const double stop2 = from < to ? to - 0.75 * view : to + 0.75 * view;
        return static_cast<int>(completion < 0.33
                                    ? ease(from, stop1, completion / 0.33)
                                    : ease(stop2, to, (completion - 0.33) / (1 - 0.33)));
    }
    return static_cast<int>(ease(from, to, completion));
}

TEST(SmoothScroll, AgreesWithAPlainReferenceAtRandomTimes) {
    std::mt19937 random(243);
    std::uniform_int_distribution<int> position(0, 20000);
    std::uniform_int_distribution<int> view(1, 2000);
    std::uniform_int_distribution<std::int64_t> time(0, 160);
    for (int run = 0; run < 20000; ++run) {
        const ScrollPosition from{position(random), position(random)};
        const ScrollPosition to{position(random), position(random)};
        const int width = view(random);
        const int height = view(random);
        const std::int64_t asked = position(random);
        const std::int64_t after = time(random);
        const SmoothScroll scroll(from, to, width, height, asked);
        const double completion = static_cast<double>(after + 10) / 135;
        const ScrollPosition at = scroll.at(asked + after);
        ASSERT_EQ(scroll.done(asked + after), completion >= 1) << run;
        if (completion >= 1) {
            ASSERT_EQ(at, to) << run;
            continue;
        }
        // The curve is computed in another order, which can move a value across a whole number.
        ASSERT_NEAR(at.left, reference(from.left, to.left, width, completion), 1) << run;
        ASSERT_NEAR(at.top, reference(from.top, to.top, height, completion), 1) << run;
    }
}

} // namespace
