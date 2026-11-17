#include "anvil/core/text_file.hpp"

#include <gtest/gtest.h>

using namespace anvil::core;
using anvil::ErrorCode;

TEST(LineEndingText, HasTheCharactersAndWidthOfEachStyle) {
    EXPECT_EQ(line_ending_text(LineEnding::lf), "\n");
    EXPECT_EQ(line_ending_text(LineEnding::crlf), "\r\n");
    EXPECT_EQ(line_ending_text(LineEnding::cr), "\r");
    EXPECT_EQ(line_ending_width(LineEnding::lf), 1u);
    EXPECT_EQ(line_ending_width(LineEnding::crlf), 2u);
    EXPECT_EQ(line_ending_width(LineEnding::cr), 1u);
}

TEST(DetectLineEnding, ChoosesTheMostFrequentStyle) {
    EXPECT_EQ(detect_line_ending("a\r\nb\r\nc\n"), LineEnding::crlf);
    EXPECT_EQ(detect_line_ending("a\nb\nc\r\n"), LineEnding::lf);
    EXPECT_EQ(detect_line_ending("a\rb\rc\n"), LineEnding::cr);
}

TEST(DetectLineEnding, DefaultsToLfWithoutLineBreaksOrOnTies) {
    EXPECT_EQ(detect_line_ending("no breaks"), LineEnding::lf);
    EXPECT_EQ(detect_line_ending(""), LineEnding::lf);
    EXPECT_EQ(detect_line_ending("a\r\nb\n"), LineEnding::lf);
}

TEST(ApplyLineEnding, ConvertsEveryLineBreak) {
    EXPECT_EQ(apply_line_ending("a\nb\n", LineEnding::crlf), "a\r\nb\r\n");
    EXPECT_EQ(apply_line_ending("a\nb\n", LineEnding::cr), "a\rb\r");
    EXPECT_EQ(apply_line_ending("a\nb\n", LineEnding::lf), "a\nb\n");
}

TEST(DecodeFile, NormalizesTextAndRemembersTheLineEnding) {
    const auto file = decode_file("one\r\ntwo\r\nthree");
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->text.to_string(), "one\ntwo\nthree");
    EXPECT_EQ(file->line_ending, LineEnding::crlf);
    EXPECT_FALSE(file->has_bom);
}

TEST(DecodeFile, StripsAndRemembersTheByteOrderMark) {
    const auto file = decode_file("\xEF\xBB\xBFhello");
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->text.to_string(), "hello");
    EXPECT_TRUE(file->has_bom);
}

TEST(DecodeFile, RejectsInvalidUtf8) {
    const auto file = decode_file("ok\xFF");
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().code, ErrorCode::invalid_encoding);
}

TEST(EncodeFile, RestoresTheLineEndingAndByteOrderMark) {
    const Text text = *Text::from_utf8("a\nb");
    EXPECT_EQ(encode_file(text, LineEnding::crlf, true), "\xEF\xBB\xBF"
                                                         "a\r\nb");
    EXPECT_EQ(encode_file(text, LineEnding::lf, false), "a\nb");
}

TEST(DecodeFile, RoundTripsThroughEncodeFile) {
    for (const std::string original : {"x\r\ny\r\n", "\xEF\xBB\xBFx\ny", "a\rb\r", "plain", ""}) {
        const auto file = decode_file(original);
        ASSERT_TRUE(file.has_value());
        EXPECT_EQ(encode_file(file->text, file->line_ending, file->has_bom), original);
    }
}
