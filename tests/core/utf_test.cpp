#include "anvil/core/utf.hpp"

#include <gtest/gtest.h>

using anvil::ErrorCode;
using anvil::core::utf16_length;

TEST(Utf16Length, EmptyStringHasNoUnits) {
    EXPECT_EQ(utf16_length(""), 0u);
}

TEST(Utf16Length, AsciiIsOneUnitPerByte) {
    EXPECT_EQ(utf16_length("anvil"), 5u);
}

TEST(Utf16Length, TwoAndThreeByteSequencesAreOneUnit) {
    EXPECT_EQ(utf16_length("\xC3\xA9"), 1u);     // U+00E9
    EXPECT_EQ(utf16_length("\xE2\x82\xAC"), 1u); // U+20AC
}

TEST(Utf16Length, SupplementaryPlaneIsSurrogatePair) {
    EXPECT_EQ(utf16_length("\xF0\x9F\x98\x80"), 2u); // U+1F600
}

TEST(Utf16Length, MixedContentSumsUnits) {
    EXPECT_EQ(utf16_length("a\xC3\xA9\xF0\x9F\x98\x80z"), 5u);
}

TEST(Utf16Length, RejectsMalformedInput) {
    for (const char* bad : {
             "\x80",             // lone continuation byte
             "\xE2\x82",         // truncated sequence
             "\xC0\x80",         // overlong encoding
             "\xE0\x80\x80",     // overlong encoding
             "\xED\xA0\x80",     // UTF-16 surrogate
             "\xF4\x90\x80\x80", // above U+10FFFF
             "\xFF",             // invalid lead byte
         }) {
        const auto result = utf16_length(bad);
        ASSERT_FALSE(result.has_value()) << "accepted malformed input";
        EXPECT_EQ(result.error().code, ErrorCode::invalid_encoding);
    }
}

TEST(Utf16Length, ErrorReportsOffsetOfFirstInvalidSequence) {
    const auto result = utf16_length("ab\x80");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("byte 2"), std::string::npos);
}
