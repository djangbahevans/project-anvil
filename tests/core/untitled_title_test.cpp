#include "anvil/core/untitled_title.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>

using anvil::core::Text;
using anvil::core::untitled_title;

namespace {

std::optional<std::string> title(const std::string& text) {
    return untitled_title(*Text::from_utf8(text));
}

} // namespace

TEST(UntitledTitle, IsTheFirstLine) {
    EXPECT_EQ(title("A list\nof things"), "A list");
    EXPECT_EQ(title("A list\r\nof things"), "A list");
    EXPECT_EQ(title("one"), "one");
}

TEST(UntitledTitle, HasNoWhiteSpaceAtItsEndsAndARunOfItAsOneSpace) {
    EXPECT_EQ(title("  \tbuy \t  milk \t "), "buy milk");
    // A no-break space and an ideographic space.
    EXPECT_EQ(title("\xC2\xA0"
                    "a\xE3\x80\x80\xE3\x80\x80"
                    "b\xC2\xA0"),
              "a b");
}

TEST(UntitledTitle, LosesTheRightToLeftOverride) {
    EXPECT_EQ(title("abc\xE2\x80\xAE"
                    "def"),
              "abcdef");
}

TEST(UntitledTitle, IsNoneForALineWithoutAWord) {
    EXPECT_EQ(title(""), std::nullopt);
    EXPECT_EQ(title("   \t"), std::nullopt);
    EXPECT_EQ(title("\ntext"), std::nullopt);
    EXPECT_EQ(title("// {}"), std::nullopt);
    EXPECT_EQ(title("`~!@#$%^&*()-=+[{]}\\|;:'\",.<>/?"), std::nullopt);
    // An underscore and a letter of another script are not separators.
    EXPECT_EQ(title("_"), "_");
    EXPECT_EQ(title("# \xC3\xA9t\xC3\xA9"), "# \xC3\xA9t\xC3\xA9");
}

TEST(UntitledTitle, IsCutAfterFortyUtf16UnitsBetweenGraphemeClusters) {
    const std::string forty(40, 'a');
    EXPECT_EQ(title(forty), forty);
    EXPECT_EQ(title(forty + "b"), forty);
    // An emoji takes two units, so it does not fit after 39.
    const std::string almost(39, 'a');
    EXPECT_EQ(title(almost + "\xF0\x9F\x98\x80" + "b"), almost);
    EXPECT_EQ(title(std::string(38, 'a') + "\xF0\x9F\x98\x80" + "b"),
              std::string(38, 'a') + "\xF0\x9F\x98\x80");
    // A letter and its combining accent are not parted.
    EXPECT_EQ(title(almost + "e\xCC\x81"), almost);
}

TEST(UntitledTitle, ComesFromTheStartOfALongLineOnly) {
    // White space counts among the units that are read.
    EXPECT_EQ(title(std::string(500, ' ') + "far"), std::nullopt);
    EXPECT_EQ(title(std::string(300, ' ') + "near"), "near");
    std::string wide;
    for (int i = 0; i < 2000; ++i) {
        wide += "\xF0\x9F\x98\x80";
    }
    ASSERT_TRUE(title(wide).has_value());
    EXPECT_EQ(title(wide)->size(), 20u * 4);
}
