#include "anvil/core/text_buffer.hpp"

#include <gtest/gtest.h>

using namespace anvil::core;
using anvil::ErrorCode;

namespace {

TextBuffer buffer_of(std::string_view utf8) {
    return TextBuffer(*Text::from_utf8(utf8));
}

} // namespace

TEST(TextBuffer, ReplaceUpdatesTheTextAndDescribesTheChange) {
    TextBuffer buffer = buffer_of("one\ntwo\nthree");
    const auto change = buffer.replace(4, 9, "2\nxx\r\nyy");
    ASSERT_TRUE(change.has_value()) << change.error().message;

    EXPECT_EQ(buffer.snapshot().to_string(), "one\n2\nxx\nyyhree");
    EXPECT_EQ(change->offset, 4u);
    EXPECT_EQ(change->removed_bytes, 5u);
    EXPECT_EQ(change->range, (Range{{1, 0}, {2, 1}}));
    EXPECT_EQ(change->removed_utf16, 5u);
    EXPECT_EQ(change->text, "2\nxx\nyy");
}

TEST(TextBuffer, ChangeLengthsCountUtf16Units) {
    TextBuffer buffer = buffer_of("a\xF0\x9F\x98\x80z");
    const auto change = buffer.erase(1, 5);
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(change->removed_bytes, 4u);
    EXPECT_EQ(change->removed_utf16, 2u);
    EXPECT_EQ(change->range, (Range{{0, 1}, {0, 3}}));
    EXPECT_EQ(buffer.snapshot().to_string(), "az");
}

TEST(TextBuffer, InsertAndEraseAreReplaceShorthands) {
    TextBuffer buffer = buffer_of("ac");
    ASSERT_TRUE(buffer.insert(1, "b").has_value());
    EXPECT_EQ(buffer.snapshot().to_string(), "abc");
    ASSERT_TRUE(buffer.erase(0, 2).has_value());
    EXPECT_EQ(buffer.snapshot().to_string(), "c");
}

TEST(TextBuffer, FailedEditsLeaveTheTextUnchanged) {
    TextBuffer buffer = buffer_of("abc");
    const auto change = buffer.replace(2, 9, "x");
    ASSERT_FALSE(change.has_value());
    EXPECT_EQ(change.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(buffer.snapshot().to_string(), "abc");
}

TEST(TextBuffer, ReplaceByRangeConvertsAndClampsPositions) {
    TextBuffer buffer = buffer_of("ab\ncd");
    const auto change = buffer.replace(Range{{0, 1}, {1, 99}}, "-");
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(buffer.snapshot().to_string(), "a-");
    EXPECT_EQ(change->offset, 1u);
    EXPECT_EQ(change->removed_bytes, 4u);
}

TEST(TextBuffer, SnapshotsKeepTheirTextAcrossEdits) {
    TextBuffer buffer = buffer_of("before");
    const Text snapshot = buffer.snapshot();
    ASSERT_TRUE(buffer.replace(0, 6, "after").has_value());
    EXPECT_EQ(snapshot.to_string(), "before");
    EXPECT_EQ(buffer.snapshot().to_string(), "after");
}

TEST(TextBuffer, ResetRestoresASnapshot) {
    TextBuffer buffer = buffer_of("one");
    const Text first = buffer.snapshot();
    ASSERT_TRUE(buffer.insert(3, " two").has_value());
    buffer.reset(first);
    EXPECT_EQ(buffer.snapshot().to_string(), "one");
}
