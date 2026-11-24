#include "anvil/core/go_to_line.hpp"

#include <gtest/gtest.h>
#include <random>

using namespace anvil::core;

namespace {

Text text_of(std::string_view bytes) {
    return Text::from_utf8(bytes).value();
}

const Text sample = text_of("first\nsecond line\n\tindented\n\nlast");

} // namespace

TEST(GoToLine, WithoutANumberItNamesNoLine) {
    for (const char* input : {"", " ", "abc", ":5", ",", "#3"}) {
        const GoToLine parsed = parse_go_to_line(sample, 4, input);
        EXPECT_FALSE(parsed.has_line) << input;
        EXPECT_EQ(parsed.line_count, 5U);
    }
}

TEST(GoToLine, ALineNumberCountsFromOneAndIsKeptWithinTheText) {
    EXPECT_EQ(parse_go_to_line(sample, 4, "1").line, 0U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "3").line, 2U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "0").line, 0U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "5").line, 4U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "99").line, 4U);
    EXPECT_TRUE(parse_go_to_line(sample, 4, "99").has_line);
}

TEST(GoToLine, ANegativeLineCountsFromTheEnd) {
    EXPECT_EQ(parse_go_to_line(sample, 4, "-1").line, 4U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "-2").line, 3U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "-5").line, 0U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "-99").line, 0U);
}

TEST(GoToLine, TheNumberIsReadLikeParseIntAndWhateverFollowsItIsIgnored) {
    EXPECT_EQ(parse_go_to_line(sample, 4, " 2").line, 1U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "+2").line, 1U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "2abc").line, 1U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "2.9").line, 1U);
    EXPECT_FALSE(parse_go_to_line(sample, 4, "-").has_line);
}

TEST(GoToLine, ALineAloneGoesToItsStart) {
    const GoToLine parsed = parse_go_to_line(sample, 4, "2");
    EXPECT_FALSE(parsed.has_separator);
    EXPECT_FALSE(parsed.has_column);
    EXPECT_EQ(parsed.target, (Position{1, 0}));
}

TEST(GoToLine, AColonCommaOrHashSeparatesTheColumn) {
    for (const char* input : {"2:4", "2,4", "2#4"}) {
        const GoToLine parsed = parse_go_to_line(sample, 4, input);
        EXPECT_TRUE(parsed.has_separator) << input;
        EXPECT_TRUE(parsed.has_column) << input;
        EXPECT_EQ(parsed.column, 4U) << input;
        EXPECT_EQ(parsed.target, (Position{1, 3})) << input;
    }
}

TEST(GoToLine, ASeparatorWithoutAColumnNamesNoColumn) {
    for (const char* input : {"2:", "2:x", "2::"}) {
        const GoToLine parsed = parse_go_to_line(sample, 4, input);
        EXPECT_TRUE(parsed.has_separator) << input;
        EXPECT_FALSE(parsed.has_column) << input;
        EXPECT_EQ(parsed.target, (Position{1, 0})) << input;
    }
}

TEST(GoToLine, TheColumnIsKeptWithinTheLineAndCountsFromTheEndWhenNegative) {
    EXPECT_EQ(parse_go_to_line(sample, 4, "2:0").column, 1U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "2:999").column, 12U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "2:999").target, (Position{1, 11}));
    EXPECT_EQ(parse_go_to_line(sample, 4, "2:999").max_column, 12U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "2:-1").column, 11U);
    EXPECT_EQ(parse_go_to_line(sample, 4, "2:-1").target, (Position{1, 10}));
    EXPECT_EQ(parse_go_to_line(sample, 4, "2:-999").column, 1U);
}

TEST(GoToLine, AnEmptyLineHasOneColumn) {
    const GoToLine parsed = parse_go_to_line(sample, 4, "4:7");
    EXPECT_EQ(parsed.max_column, 1U);
    EXPECT_EQ(parsed.column, 1U);
    EXPECT_EQ(parsed.target, (Position{3, 0}));
}

TEST(GoToLine, AColumnCountsATabAsFarAsTheNextTabStop) {
    // "\tindented" with tab size 4: the tab covers columns 1-4, so 'i' is at column 5.
    EXPECT_EQ(parse_go_to_line(sample, 4, "3:5").target, (Position{2, 1}));
    EXPECT_EQ(parse_go_to_line(sample, 4, "3:6").target, (Position{2, 2}));
    EXPECT_EQ(parse_go_to_line(sample, 8, "3:9").target, (Position{2, 1}));
    EXPECT_EQ(parse_go_to_line(sample, 4, "3:1").target, (Position{2, 0}));
    EXPECT_EQ(parse_go_to_line(sample, 4, "3:999").max_column, 13U);
}

TEST(GoToLine, ATabAfterTextEndsAtTheNextTabStopOfTheColumnItIsAt) {
    const Text text = text_of("ab\tx");
    EXPECT_EQ(parse_go_to_line(text, 4, "1:5").target, (Position{0, 3}));
    EXPECT_EQ(parse_go_to_line(text, 4, "1:5").max_column, 6U);
    EXPECT_EQ(parse_go_to_line(text, 8, "1:9").target, (Position{0, 3}));
}

TEST(GoToLine, AColumnInsideATabFallsOnTheNearerEdge) {
    EXPECT_EQ(parse_go_to_line(sample, 4, "3:2").target, (Position{2, 0}));
    EXPECT_EQ(parse_go_to_line(sample, 4, "3:3").target, (Position{2, 0}));
    EXPECT_EQ(parse_go_to_line(sample, 4, "3:4").target, (Position{2, 1}));
}

TEST(GoToLine, ColumnsAreUtf16CodeUnitsOfTheTarget) {
    const Text emoji = text_of("a\xF0\x9F\x98\x80z");
    EXPECT_EQ(parse_go_to_line(emoji, 4, "1:2").target, (Position{0, 1}));
    EXPECT_EQ(parse_go_to_line(emoji, 4, "1:3").target, (Position{0, 3}));
    EXPECT_EQ(parse_go_to_line(emoji, 4, "1:4").target, (Position{0, 4}));
    const Text accent = text_of("\xC3\xA9x");
    EXPECT_EQ(parse_go_to_line(accent, 4, "1:2").target, (Position{0, 1}));
}

TEST(GoToLine, ATextWithoutLineBreaksHasOneLine) {
    const Text one = text_of("only");
    EXPECT_EQ(parse_go_to_line(one, 4, "7").line, 0U);
    EXPECT_EQ(parse_go_to_line(one, 4, "7").line_count, 1U);
    EXPECT_EQ(parse_go_to_line(text_of(""), 4, "1:5").target, (Position{0, 0}));
}

namespace {

// The line and column of VS Code's rules, written for lines of one character width without tabs.
Position reference_target(const std::vector<std::string>& lines, long long line, long long column) {
    const auto count = static_cast<long long>(lines.size());
    line = line >= 0 ? line : count + 1 + line;
    line = std::min(std::max(1LL, line), count);
    const auto width = static_cast<long long>(lines[static_cast<std::size_t>(line - 1)].size());
    const long long max_column = width + 1;
    column = column >= 0 ? column : max_column + column;
    column = std::min(std::max(1LL, column), max_column);
    return {static_cast<std::size_t>(line - 1), static_cast<std::size_t>(column - 1)};
}

} // namespace

TEST(GoToLineModel, ForLinesWithoutTabsTheTargetFollowsTheRulesOfVsCode) {
    std::mt19937 random(5);
    for (int round = 0; round < 500; ++round) {
        std::vector<std::string> lines;
        std::string bytes;
        const std::size_t count = 1 + random() % 6;
        for (std::size_t i = 0; i < count; ++i) {
            lines.emplace_back(random() % 8, 'x');
            bytes += lines.back() + (i + 1 < count ? "\n" : "");
        }
        const Text text = text_of(bytes);
        for (int check = 0; check < 20; ++check) {
            const auto line = static_cast<long long>(random() % 15) - 7;
            const auto column = static_cast<long long>(random() % 21) - 10;
            const std::string input = std::to_string(line) + ":" + std::to_string(column);
            ASSERT_EQ(parse_go_to_line(text, 4, input).target,
                      reference_target(lines, line, column))
                << bytes << " " << input;
        }
    }
}
