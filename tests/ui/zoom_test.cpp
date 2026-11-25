#include "anvil/ui/zoom.hpp"

#include <QSignalSpy>
#include <cmath>
#include <gtest/gtest.h>

namespace {

using anvil::ui::Zoom;

TEST(ZoomObjectTest, ItStartsAtLevelZero) {
    const Zoom zoom;
    EXPECT_EQ(zoom.level(), 0);
    EXPECT_EQ(zoom.factor(), 1.0);
    EXPECT_EQ(zoom.scaled(35), 35);
}

TEST(ZoomObjectTest, ScaledMatchesAModelOverTheLevels) {
    Zoom zoom;
    for (double level = -8; level <= 8; level += 1) {
        zoom.set_level(level);
        for (int size = 1; size <= 100; ++size) {
            const long expected = std::max(1L, std::lround(size * std::pow(1.2, level)));
            ASSERT_EQ(zoom.scaled(size), expected) << size << " at " << level;
        }
        EXPECT_EQ(zoom.scaled(0), 0);
    }
}

TEST(ZoomObjectTest, ALevelOutsideTheRangeIsClamped) {
    Zoom zoom;
    zoom.set_level(20);
    EXPECT_EQ(zoom.level(), 8);
    zoom.set_level(-20);
    EXPECT_EQ(zoom.level(), -8);
}

TEST(ZoomObjectTest, ChangedIsEmittedWhenTheLevelChangesAndOnlyThen) {
    Zoom zoom;
    QSignalSpy spy(&zoom, &Zoom::changed);
    zoom.set_level(0);
    EXPECT_EQ(spy.count(), 0);
    zoom.set_level(2);
    EXPECT_EQ(spy.count(), 1);
    zoom.set_level(2);
    EXPECT_EQ(spy.count(), 1);
    zoom.set_level(100);
    EXPECT_EQ(spy.count(), 2);
    zoom.set_level(8);
    EXPECT_EQ(spy.count(), 2) << "the clamped level is the one already set";
}

TEST(ZoomObjectTest, TheFactorFollowsTheLevel) {
    Zoom zoom;
    zoom.set_level(3);
    EXPECT_DOUBLE_EQ(zoom.factor(), std::pow(1.2, 3));
}

} // namespace
