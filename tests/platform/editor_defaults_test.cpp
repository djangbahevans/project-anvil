#include "anvil/platform/editor_defaults.hpp"

#include <gtest/gtest.h>

using anvil::core::LineEnding;

TEST(EditorDefaults, FollowVsCodesDefaultsForThePlatform) {
    const auto defaults = anvil::platform::editor_defaults();
    EXPECT_FALSE(defaults.font_family.empty());
#if defined(__APPLE__)
    EXPECT_EQ(defaults.font_size, 12);
    EXPECT_EQ(defaults.font_family, "Menlo, Monaco, 'Courier New', monospace");
#else
    EXPECT_EQ(defaults.font_size, 14);
#endif
#if defined(__linux__)
    EXPECT_EQ(defaults.font_family, "'Droid Sans Mono', monospace");
#endif
    EXPECT_EQ(defaults.auto_line_ending, LineEnding::lf);
}
