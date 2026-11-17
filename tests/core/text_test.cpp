#include "anvil/core/text.hpp"

#include <gtest/gtest.h>

using namespace anvil::core;
using anvil::ErrorCode;

namespace {

Text text_of(std::string_view utf8) {
    auto text = Text::from_utf8(utf8);
    EXPECT_TRUE(text.has_value());
    return text ? *text : Text{};
}

constexpr const char* astral = "\xF0\x9F\x98\x80"; // U+1F600, two UTF-16 units
constexpr const char* euro = "\xE2\x82\xAC";       // U+20AC, one UTF-16 unit

} // namespace

TEST(Text, EmptyTextHasOneEmptyLine) {
    const Text text;
    EXPECT_EQ(text.size(), 0u);
    EXPECT_TRUE(text.empty());
    EXPECT_EQ(text.line_count(), 1u);
    EXPECT_EQ(text.line(0), "");
    EXPECT_EQ(text.position_at(0), (Position{0, 0}));
    EXPECT_EQ(text.offset_at({0, 0}), 0u);
    EXPECT_EQ(text.to_string(), "");
    EXPECT_FALSE(text.check_invariants().has_value());
}

TEST(Text, RejectsMalformedUtf8) {
    const auto text = Text::from_utf8("ab\xC0\x80");
    ASSERT_FALSE(text.has_value());
    EXPECT_EQ(text.error().code, ErrorCode::invalid_encoding);
}

TEST(Text, NormalizesLineEndingsOnLoad) {
    const Text text = text_of("a\r\nb\rc\nd");
    EXPECT_EQ(text.to_string(), "a\nb\nc\nd");
    EXPECT_EQ(text.line_count(), 4u);
}

TEST(NormalizeLineEndings, HandlesRunsAndTrailingCarriageReturns) {
    EXPECT_EQ(normalize_line_endings("\r\r\n\n\r"), "\n\n\n\n");
    EXPECT_EQ(normalize_line_endings("no breaks"), "no breaks");
}

TEST(Text, SplitsIntoLines) {
    const Text text = text_of("a\nbc\n\nd");
    ASSERT_EQ(text.line_count(), 4u);
    EXPECT_EQ(text.line(0), "a");
    EXPECT_EQ(text.line(1), "bc");
    EXPECT_EQ(text.line(2), "");
    EXPECT_EQ(text.line(3), "d");
    EXPECT_EQ(text.line(4), "");
    EXPECT_EQ(text.line_start(1), 2u);
    EXPECT_EQ(text.line_end(1), 4u);
    EXPECT_EQ(text.line_start(3), 6u);
    EXPECT_EQ(text.line_end(3), text.size());
    EXPECT_EQ(text.line_start(9), text.size());
}

TEST(Text, TrailingLineBreakStartsAnEmptyLine) {
    const Text text = text_of("a\n");
    EXPECT_EQ(text.line_count(), 2u);
    EXPECT_EQ(text.line(1), "");
    EXPECT_EQ(text.position_at(2), (Position{1, 0}));
}

TEST(Text, ColumnsCountUtf16CodeUnits) {
    const Text text = text_of(std::string("a") + astral + euro + "b");
    // bytes: a=0, astral=1..5, euro=5..8, b=8
    EXPECT_EQ(text.size(), 9u);
    EXPECT_EQ(text.utf16_length(), 5u);
    EXPECT_EQ(text.position_at(0), (Position{0, 0}));
    EXPECT_EQ(text.position_at(1), (Position{0, 1}));
    EXPECT_EQ(text.position_at(5), (Position{0, 3}));
    EXPECT_EQ(text.position_at(8), (Position{0, 4}));
    EXPECT_EQ(text.position_at(9), (Position{0, 5}));
    EXPECT_EQ(text.offset_at({0, 3}), 5u);
    EXPECT_EQ(text.offset_at({0, 4}), 8u);
    EXPECT_EQ(text.utf16_offset(5), 3u);
}

TEST(Text, ColumnInsideASurrogatePairMapsToTheStartOfThePair) {
    const Text text = text_of(std::string("a") + astral + "b");
    EXPECT_EQ(text.offset_at({0, 2}), 1u);
    EXPECT_EQ(text.offset_at({0, 3}), 5u);
}

TEST(Text, OffsetAtClampsToTheTextAndToTheLine) {
    const Text text = text_of("ab\ncd");
    EXPECT_EQ(text.offset_at({0, 99}), 2u);
    EXPECT_EQ(text.offset_at({1, 99}), 5u);
    EXPECT_EQ(text.offset_at({7, 0}), 5u);
    EXPECT_EQ(text.position_at(99), (Position{1, 2}));
}

TEST(Text, KnowsCharacterBoundaries) {
    const Text text = text_of(std::string("a") + euro);
    EXPECT_TRUE(text.is_char_boundary(0));
    EXPECT_TRUE(text.is_char_boundary(1));
    EXPECT_FALSE(text.is_char_boundary(2));
    EXPECT_FALSE(text.is_char_boundary(3));
    EXPECT_TRUE(text.is_char_boundary(4));
    EXPECT_FALSE(text.is_char_boundary(5));
}

TEST(Text, SliceClampsToTheEnd) {
    const Text text = text_of("hello world");
    EXPECT_EQ(text.slice(6, 100), "world");
    EXPECT_EQ(text.slice(3, 4), "lo w");
    EXPECT_EQ(text.slice(50, 4), "");
}

TEST(Text, ReplacedProducesNewTextAndLeavesTheOriginal) {
    const Text before = text_of("hello world");
    const auto after = before.replaced(6, 11, "there");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->to_string(), "hello there");
    EXPECT_EQ(before.to_string(), "hello world");
}

TEST(Text, ReplacedSupportsInsertionAndDeletion) {
    const Text text = text_of("abc");
    EXPECT_EQ(text.replaced(1, 1, "XY")->to_string(), "aXYbc");
    EXPECT_EQ(text.replaced(0, 3, "")->to_string(), "");
    EXPECT_EQ(text.replaced(3, 3, "d")->to_string(), "abcd");
    EXPECT_EQ(Text{}.replaced(0, 0, "x")->to_string(), "x");
}

TEST(Text, ReplacedNormalizesLineEndings) {
    const auto text = Text{}.replaced(0, 0, "a\r\nb\rc");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->to_string(), "a\nb\nc");
}

TEST(Text, ReplacedRejectsInvalidRanges) {
    const Text text = text_of(std::string("a") + euro);
    for (auto [start, end] : {std::pair<std::size_t, std::size_t>{2, 1}, {0, 9}, {2, 4}, {0, 2}}) {
        const auto result = text.replaced(start, end, "x");
        ASSERT_FALSE(result.has_value()) << start << ".." << end;
        EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
    }
}

TEST(Text, ReplacedRejectsMalformedReplacement) {
    const auto result = text_of("abc").replaced(0, 1, "\xFF");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::invalid_encoding);
}

TEST(Text, SnapshotsAreIndependentOfLaterEdits) {
    const Text original = text_of(std::string(5000, 'x'));
    const Text copy = original;
    const auto edited = original.replaced(100, 4000, "y");
    ASSERT_TRUE(edited.has_value());
    EXPECT_EQ(copy.size(), 5000u);
    EXPECT_EQ(edited->size(), 1101u);
    EXPECT_EQ(copy.to_string(), std::string(5000, 'x'));
}

TEST(Text, LargeTextIsBalancedAndChunked) {
    std::string big;
    for (int i = 0; i < 40000; ++i) {
        big += "line " + std::to_string(i) + " \xE2\x82\xAC\n";
    }
    const Text text = text_of(big);
    EXPECT_FALSE(text.check_invariants().has_value()) << *text.check_invariants();
    EXPECT_EQ(text.size(), big.size());
    EXPECT_EQ(text.line_count(), 40001u);
    EXPECT_EQ(text.line(39999), "line 39999 \xE2\x82\xAC");
    EXPECT_EQ(text.to_string(), big);
}
