#include "anvil/core/char_class.hpp"

#include <gtest/gtest.h>

using namespace anvil::core;

TEST(CharClassTest, ClassifiesAscii) {
    EXPECT_EQ(classify(' '), CharClass::space);
    EXPECT_EQ(classify('\t'), CharClass::space);
    EXPECT_EQ(classify('\n'), CharClass::space);
    for (const char32_t c : std::u32string_view(U"azAZ09_")) {
        EXPECT_EQ(classify(c), CharClass::word) << static_cast<unsigned>(c);
    }
    for (const char32_t c : std::u32string_view(U"-.(){}$/@`~")) {
        EXPECT_EQ(classify(c), CharClass::punctuation) << static_cast<unsigned>(c);
    }
}

TEST(CharClassTest, EveryNonAsciiCharacterIsAWordCharacter) {
    for (const char32_t c : std::u32string_view(U"é€😀—")) {
        EXPECT_TRUE(is_word_character(c)) << static_cast<unsigned>(c);
    }
}

TEST(CharClassTest, DecodesCodePointsOfEveryLength) {
    const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    EXPECT_EQ(decode_code_point(text, 0), U'a');
    EXPECT_EQ(decode_code_point(text, 1), U'é');
    EXPECT_EQ(decode_code_point(text, 3), U'€');
    EXPECT_EQ(decode_code_point(text, 6), U'😀');
}
