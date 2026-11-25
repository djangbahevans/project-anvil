#include "anvil/core/zoom.hpp"

#include <cmath>
#include <gtest/gtest.h>

namespace {

using anvil::core::max_zoom_level;
using anvil::core::min_zoom_level;
using anvil::core::scale_for_zoom;
using anvil::core::zoom_factor;

TEST(ZoomTest, TheFactorIsOnePointTwoToTheLevel) {
    EXPECT_DOUBLE_EQ(zoom_factor(0), 1.0);
    EXPECT_DOUBLE_EQ(zoom_factor(1), 1.2);
    EXPECT_DOUBLE_EQ(zoom_factor(-1), 1.0 / 1.2);
    EXPECT_NEAR(zoom_factor(8), 4.29981696, 1e-8);
    EXPECT_NEAR(zoom_factor(-8), 0.23256804, 1e-8);
    EXPECT_NEAR(zoom_factor(0.5), std::sqrt(1.2), 1e-12);
}

TEST(ZoomTest, TheRangeIsEightLevelsEitherWay) {
    EXPECT_EQ(min_zoom_level, -8);
    EXPECT_EQ(max_zoom_level, 8);
}

TEST(ZoomTest, AScaledSizeIsTheRoundedProductOfTheSizeAndTheFactor) {
    for (int size = 1; size <= 200; ++size) {
        for (double level = -8; level <= 8; level += 0.5) {
            const long expected = std::max(1L, std::lround(size * std::pow(1.2, level)));
            ASSERT_EQ(scale_for_zoom(size, level), expected) << size << " at " << level;
        }
    }
}

TEST(ZoomTest, TheSizesOfLevelOneAreTwentyPercentLarger) {
    EXPECT_EQ(scale_for_zoom(35, 1), 42);
    EXPECT_EQ(scale_for_zoom(22, 1), 26);
    EXPECT_EQ(scale_for_zoom(14, 0), 14);
}

TEST(ZoomTest, ASizeAboveZeroNeverVanishes) {
    EXPECT_EQ(scale_for_zoom(1, -8), 1);
    EXPECT_EQ(scale_for_zoom(2, -8), 1);
}

TEST(ZoomTest, ASizeOfZeroOrLessIsLeftAlone) {
    EXPECT_EQ(scale_for_zoom(0, 3), 0);
    EXPECT_EQ(scale_for_zoom(-5, 3), -5);
}

} // namespace
