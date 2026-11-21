#include "anvil/core/jsonc.hpp"

#include <gtest/gtest.h>

using anvil::ErrorCode;
using anvil::core::is_blank_jsonc;
using anvil::core::parse_jsonc;
using nlohmann::json;

namespace {

struct Valid {
    const char* name;
    std::string text;
    json expected;
};

class JsoncValid : public ::testing::TestWithParam<Valid> {};

TEST_P(JsoncValid, Parses) {
    const auto result = parse_jsonc(GetParam().text);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, GetParam().expected);
}

INSTANTIATE_TEST_SUITE_P(
    Table, JsoncValid,
    ::testing::Values(
        Valid{"Object", R"j({"a": 1, "b": [true, false, null], "c": "x"})j",
              json{{"a", 1}, {"b", {true, false, nullptr}}, {"c", "x"}}},
        Valid{"LineComment", "{\n// a comment\n\"a\": 1 // trailing\n}\n", json{{"a", 1}}},
        Valid{"BlockComment", "/* head */ {\"a\" /* in */ : /* between */ 1 }", json{{"a", 1}}},
        Valid{"BlockCommentAcrossLines", "{\n/* one\n two */ \"a\": 1}", json{{"a", 1}}},
        Valid{"TrailingCommaInObject", R"j({"a": 1, "b": 2,})j", json{{"a", 1}, {"b", 2}}},
        Valid{"TrailingCommaInArray", "[1, 2, ]", json::array({1, 2})},
        Valid{"TrailingCommaBeforeComment", "[1, // one\n 2, // two\n]", json::array({1, 2})},
        Valid{"NestedTrailingCommas", R"j({"a": [{"b": 1,},],})j",
              json{{"a", json::array({json{{"b", 1}}})}}},
        Valid{"CommentMarkersInsideStrings",
              R"j({"url": "http://example.com/*x*/", "c": "// not a comment"})j",
              json{{"url", "http://example.com/*x*/"}, {"c", "// not a comment"}}},
        Valid{"CommaInsideStringBeforeBracket", R"j(["a,]", "b,}"])j", json::array({"a,]", "b,}"})},
        Valid{"EmptyContainers", R"j({"a": {}, "b": []})j",
              json{{"a", json::object()}, {"b", json::array()}}},
        Valid{"Escapes", "\"\\\" \\\\ \\/ \\b \\f \\n \\r \\t \\u00e9 \\ud83d\\ude00\"",
              json("\" \\ / \b \f \n \r \t \xC3\xA9 \xF0\x9F\x98\x80")},
        Valid{"Integers", "[0, -7, 42, 18446744073709551615]",
              json::array({0, -7, 42, 18446744073709551615ull})},
        Valid{"Floats", "[1.5, -0.25, 1e3, 2E-2]", json::array({1.5, -0.25, 1000.0, 0.02})},
        Valid{"ScalarDocument", "  true  ", json(true)},
        Valid{"ByteOrderMark", "\xEF\xBB\xBF{\"a\": 1}", json{{"a", 1}}},
        Valid{"RepeatedKeyKeepsTheLast", R"j({"a": 1, "a": 2})j", json{{"a", 2}}},
        Valid{"Utf8InStrings", "{\"k\": \"\xC3\xA9\xF0\x9F\x98\x80\"}",
              json{{"k", "\xC3\xA9\xF0\x9F\x98\x80"}}},
        Valid{"CarriageReturnEndsALineComment", "[1, // c\r2]", json::array({1, 2})}),
    [](const auto& info) { return std::string(info.param.name); });

struct Invalid {
    const char* name;
    std::string text;
    std::string message;
};

class JsoncInvalid : public ::testing::TestWithParam<Invalid> {};

TEST_P(JsoncInvalid, FailsAtThePosition) {
    const auto result = parse_jsonc(GetParam().text);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(result.error().message, GetParam().message);
}

INSTANTIATE_TEST_SUITE_P(
    Table, JsoncInvalid,
    ::testing::Values(
        Invalid{"Empty", "", "line 1, column 1: expected a value"},
        Invalid{"OnlyAComment", "// nothing\n", "line 2, column 1: expected a value"},
        Invalid{"UnterminatedString", "{\n  \"a\": \"abc\n}",
                "line 2, column 8: unterminated string"},
        Invalid{"UnterminatedStringAtEndOfInput", "{\"a\": \"abc",
                "line 1, column 7: unterminated string"},
        Invalid{"UnterminatedBlockComment", "{\n  /* never closed\n}",
                "line 2, column 3: unterminated comment"},
        Invalid{"UnterminatedObject", "{\"a\": 1", "line 1, column 8: unterminated object"},
        Invalid{"UnterminatedArray", "[1, 2", "line 1, column 6: unterminated array"},
        Invalid{"MissingColon", "{\"a\" 1}", "line 1, column 6: unexpected '1', expected ':'"},
        Invalid{"MissingComma", "{\n  \"a\": 1\n  \"b\": 2\n}",
                "line 3, column 3: unexpected '\"', expected ',' or '}'"},
        Invalid{"MissingValue", "{\"a\": }", "line 1, column 7: unexpected '}', expected a value"},
        Invalid{"DoubleComma", "[1,,2]", "line 1, column 4: unexpected ',', expected a value"},
        Invalid{"LeadingComma", "[,1]", "line 1, column 2: unexpected ',', expected a value"},
        Invalid{"UnquotedKey", "{a: 1}",
                "line 1, column 2: unexpected 'a', expected a property name"},
        Invalid{"SingleQuotedString", "['a']",
                "line 1, column 2: unexpected ''', expected a value"},
        Invalid{"ContentAfterTheValue", "{} {}",
                "line 1, column 4: unexpected content after the value"},
        Invalid{"BadEscape", R"j("a\qb")j", R"j(line 1, column 3: invalid escape sequence '\q')j"},
        Invalid{"ShortUnicodeEscape", R"j("\u12g4")j",
                "line 1, column 6: expected four hexadecimal digits after \\u"},
        Invalid{"LoneHighSurrogate", R"j("\ud83d x")j",
                "line 1, column 2: a high surrogate must be followed by a low surrogate"},
        Invalid{"LoneLowSurrogate", R"j("\ude00")j",
                "line 1, column 2: a low surrogate must follow a high surrogate"},
        Invalid{"ControlCharacterInString", std::string("\"a\tb\""),
                "line 1, column 3: unexpected control character U+0009 in a string"},
        Invalid{"LeadingZero", "[01]", "line 1, column 3: unexpected '1', expected ',' or ']'"},
        Invalid{"BareMinus", "[-]", "line 1, column 3: expected a digit"},
        Invalid{"MissingFraction", "[1.]",
                "line 1, column 4: expected a digit after the decimal point"},
        Invalid{"MissingExponent", "[1e]", "line 1, column 4: expected a digit in the exponent"},
        Invalid{"NumberOutOfRange", "[1e999]", "line 1, column 2: the number is out of range"},
        Invalid{"MalformedLiteral", "[tru]", "line 1, column 2: unexpected 't', expected a value"},
        Invalid{"InvalidUtf8", "{\"a\": \"\x80\"}", "line 1, column 8: invalid UTF-8"},
        Invalid{"ColumnCountsCharactersNotBytes", "{\"\xC3\xA9\xC3\xA9\": ?}",
                "line 1, column 8: unexpected '?', expected a value"},
        Invalid{"ColumnAfterTheByteOrderMarkStartsAtOne", "\xEF\xBB\xBF?",
                "line 1, column 1: unexpected '?', expected a value"},
        Invalid{"LoneCarriageReturnIsALineBreak", "{\r\"a\": ?}",
                "line 2, column 6: unexpected '?', expected a value"},
        Invalid{"CarriageReturnLinefeedIsOneLineBreak", "{\r\n\"a\": ?}",
                "line 2, column 6: unexpected '?', expected a value"}),
    [](const auto& info) { return std::string(info.param.name); });

TEST(Jsonc, RejectsNestingThatWouldOverflowTheStack) {
    const std::string deep(100000, '[');
    const auto result = parse_jsonc(deep);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("nesting is too deep"), std::string::npos);
}

TEST(Jsonc, AcceptsNestingWithinTheLimit) {
    const std::string text = std::string(200, '[') + std::string(200, ']');
    EXPECT_TRUE(parse_jsonc(text).has_value());
}

TEST(Jsonc, BlankMeansOnlyWhitespaceAndComments) {
    EXPECT_TRUE(is_blank_jsonc(""));
    EXPECT_TRUE(is_blank_jsonc("  \n\t"));
    EXPECT_TRUE(is_blank_jsonc("// only\n/* comments */\n"));
    EXPECT_FALSE(is_blank_jsonc("{}"));
    EXPECT_FALSE(is_blank_jsonc("// c\n1"));
    EXPECT_FALSE(is_blank_jsonc("/* unterminated"));
}

} // namespace
