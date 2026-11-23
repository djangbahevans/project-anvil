#include "anvil/core/jsonc.hpp"
#include "anvil/core/jsonc_edit.hpp"
#include "anvil/core/settings.hpp"

#include <gtest/gtest.h>

using anvil::ErrorCode;
using anvil::core::edit_jsonc_property;
using anvil::core::Settings;
using nlohmann::json;

namespace {

std::string set(std::string_view text, std::string_view key, const json& value) {
    const auto result = edit_jsonc_property(text, key, value);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    if (!result) {
        return {};
    }
    // Whatever the edit did, the result is settings that say what was asked.
    const auto settings = Settings::parse(*result);
    EXPECT_TRUE(settings.has_value()) << *result;
    if (settings) {
        const json* found = settings->find(key);
        EXPECT_NE(found, nullptr) << *result;
        if (found) {
            EXPECT_EQ(*found, value) << *result;
        }
    }
    return *result;
}

std::string remove(std::string_view text, std::string_view key) {
    const auto result = edit_jsonc_property(text, key, std::nullopt);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    if (!result) {
        return {};
    }
    const auto settings = Settings::parse(*result);
    EXPECT_TRUE(settings.has_value()) << *result;
    if (settings) {
        EXPECT_EQ(settings->find(key), nullptr) << *result;
    }
    return *result;
}

// --- changing a property where it is ---

TEST(EditJsoncProperty, ReplacesTheValueOfAFlatKeyAndNothingElse) {
    const std::string text =
        "{\n    // the size\n    \"editor.fontSize\": 14, // trailing\n    \"a\": 1\n}\n";
    EXPECT_EQ(set(text, "editor.fontSize", 18),
              "{\n    // the size\n    \"editor.fontSize\": 18, // trailing\n    \"a\": 1\n}\n");
}

TEST(EditJsoncProperty, ReplacesTheValueOfANestedKey) {
    EXPECT_EQ(set(R"j({ "editor": { "fontSize": 14, "tabSize": 2 } })j", "editor.fontSize", 18),
              R"j({ "editor": { "fontSize": 18, "tabSize": 2 } })j");
    EXPECT_EQ(set(R"j({"a": {"b": {"c": 1}}})j", "a.b.c", true), R"j({"a": {"b": {"c": true}}})j");
}

TEST(EditJsoncProperty, ChangesTheSpellingThatSettingsReads) {
    // A flat key wins over the nested spelling, so that is the one to change.
    EXPECT_EQ(set(R"j({"editor": {"fontSize": 1}, "editor.fontSize": 2})j", "editor.fontSize", 3),
              R"j({"editor": {"fontSize": 1}, "editor.fontSize": 3})j");
    EXPECT_EQ(set(R"j({"a.b": {"c": 1}, "a": {"b.d": 2}})j", "a.b.d", 9),
              R"j({"a.b": {"c": 1}, "a": {"b.d": 9}})j");
    EXPECT_EQ(set(R"j({"a.b": {"c": 1}, "a": {"b.d": 2}})j", "a.b.c", 9),
              R"j({"a.b": {"c": 9}, "a": {"b.d": 2}})j");
}

TEST(EditJsoncProperty, ChangesTheLastOfARepeatedKey) {
    EXPECT_EQ(set(R"j({"a": 1, "a": 2})j", "a", 3), R"j({"a": 1, "a": 3})j");
}

TEST(EditJsoncProperty, WritesTheValueAsJson) {
    EXPECT_EQ(set(R"j({"s": 1})j", "s", "Dark 2026"), R"j({"s": "Dark 2026"})j");
    EXPECT_EQ(set(R"j({"s": 1})j", "s", "quote \" slash \\ tab \t é 日本"),
              "{\"s\": \"quote \\\" slash \\\\ tab \\t é 日本\"}");
    EXPECT_EQ(set(R"j({"s": "x"})j", "s", false), R"j({"s": false})j");
    EXPECT_EQ(set(R"j({"s": "x"})j", "s", nullptr), R"j({"s": null})j");
    EXPECT_EQ(set(R"j({"s": "x"})j", "s", 2.5), R"j({"s": 2.5})j");
    EXPECT_EQ(set(R"j({"s": 1})j", "s", json::array({1, "a"})), R"j({"s": [1,"a"]})j");
}

TEST(EditJsoncProperty, ReplacesAnObjectValueWhole) {
    EXPECT_EQ(set(R"j({"a": {"b": 1, "c": [1, 2]}, "d": 0})j", "a", 5), R"j({"a": 5, "d": 0})j");
}

TEST(EditJsoncProperty, SettingTheSameValueChangesNothing) {
    const std::string text = "{\n    \"a\": 14, // c\n    \"b\": \"x\"\n}\n";
    EXPECT_EQ(set(text, "a", 14), text);
    EXPECT_EQ(set(text, "b", "x"), text);
}

// --- adding a property ---

TEST(EditJsoncProperty, AddsAFlatPropertyAfterTheLastOneWithTheSameIndentation) {
    EXPECT_EQ(set("{\n    \"a\": 1\n}\n", "b", true), "{\n    \"a\": 1,\n    \"b\": true\n}\n");
    EXPECT_EQ(set("{\n  \"a\": 1,\n  \"c\": 3\n}\n", "b", 2),
              "{\n  \"a\": 1,\n  \"c\": 3,\n  \"b\": 2\n}\n");
    EXPECT_EQ(set("{\n\t\"a\": 1\n}", "b", 2), "{\n\t\"a\": 1,\n\t\"b\": 2\n}");
}

TEST(EditJsoncProperty, TheCommaGoesBeforeACommentAtTheEndOfTheLastLine) {
    EXPECT_EQ(set("{\n    \"a\": 1 // note\n}\n", "b", 2),
              "{\n    \"a\": 1, // note\n    \"b\": 2\n}\n");
    EXPECT_EQ(set("{\n    \"a\": 1 /* note */\n}\n", "b", 2),
              "{\n    \"a\": 1, /* note */\n    \"b\": 2\n}\n");
}

TEST(EditJsoncProperty, ATrailingCommaIsKept) {
    EXPECT_EQ(set("{\n    \"a\": 1,\n}\n", "b", 2), "{\n    \"a\": 1,\n    \"b\": 2,\n}\n");
    EXPECT_EQ(set("{\n    \"a\": 1, // note\n}\n", "b", 2),
              "{\n    \"a\": 1, // note\n    \"b\": 2,\n}\n");
}

TEST(EditJsoncProperty, KeepsTheLineEndingsOfTheFile) {
    EXPECT_EQ(set("{\r\n    \"a\": 1\r\n}\r\n", "b", 2),
              "{\r\n    \"a\": 1,\r\n    \"b\": 2\r\n}\r\n");
    EXPECT_EQ(set("{\r\n}\r\n", "b", 2), "{\r\n    \"b\": 2\r\n}\r\n");
}

TEST(EditJsoncProperty, AddsToAnObjectOnOneLineOnThatLine) {
    EXPECT_EQ(set(R"j({"a": 1})j", "b", 2), R"j({"a": 1, "b": 2})j");
    EXPECT_EQ(set(R"j({"a": 1,})j", "b", 2), R"j({"a": 1, "b": 2,})j");
    EXPECT_EQ(set(R"j({ "a": 1, "c": 3 })j", "b", 2), R"j({ "a": 1, "c": 3, "b": 2 })j");
}

TEST(EditJsoncProperty, AddsToAnEmptyObject) {
    EXPECT_EQ(set("{}", "a", 1), "{\n    \"a\": 1\n}");
    EXPECT_EQ(set("{\n}\n", "a", 1), "{\n    \"a\": 1\n}\n");
    EXPECT_EQ(set("{\n    // only a comment\n}\n", "a", 1),
              "{\n    // only a comment\n    \"a\": 1\n}\n");
}

TEST(EditJsoncProperty, AddsAFlatPropertyEvenWhenTheNestedSpellingIsInUse) {
    EXPECT_EQ(
        set("{\n    \"editor\": {\n        \"fontSize\": 14\n    }\n}\n", "editor.tabSize", 2),
        "{\n    \"editor\": {\n        \"fontSize\": 14\n    },\n    \"editor.tabSize\": 2\n}\n");
    EXPECT_EQ(set(R"j({"a": 5})j", "a.b", 1), R"j({"a": 5, "a.b": 1})j");
}

TEST(EditJsoncProperty, GivesAFileWithNothingInItAnObject) {
    EXPECT_EQ(set("", "a", 1), "{\n    \"a\": 1\n}\n");
    EXPECT_EQ(set("  \n\n", "a", 1), "  \n\n{\n    \"a\": 1\n}\n");
    EXPECT_EQ(set("// my settings\n", "a", 1), "// my settings\n{\n    \"a\": 1\n}\n");
    EXPECT_EQ(set("// my settings", "a", 1), "// my settings\n{\n    \"a\": 1\n}\n");
    EXPECT_EQ(set("/* one */\r\n// two\r\n", "a", 1),
              "/* one */\r\n// two\r\n{\r\n    \"a\": 1\r\n}\r\n");
}

TEST(EditJsoncProperty, KeepsAByteOrderMark) {
    EXPECT_EQ(set("\xEF\xBB\xBF{\"a\": 1}", "a", 2), "\xEF\xBB\xBF{\"a\": 2}");
    EXPECT_EQ(set("\xEF\xBB\xBF{\"a\": 1}", "b", 2), "\xEF\xBB\xBF{\"a\": 1, \"b\": 2}");
    EXPECT_EQ(set("\xEF\xBB\xBF", "a", 1), "\xEF\xBB\xBF\n{\n    \"a\": 1\n}\n");
}

TEST(EditJsoncProperty, FollowsTheIndentationOfTheFileForAnEmptyObject) {
    EXPECT_EQ(set("{\n\t// c\n}\n", "a", 1), "{\n\t// c\n\t\"a\": 1\n}\n");
    EXPECT_EQ(set("{\n  // c\n}\n", "a", 1), "{\n  // c\n  \"a\": 1\n}\n");
}

// --- removing a property ---

TEST(EditJsoncProperty, RemovesAPropertyWithItsLineAndItsComma) {
    const std::string text = "{\n    \"a\": 1,\n    \"b\": 2,\n    \"c\": 3\n}\n";
    EXPECT_EQ(remove(text, "b"), "{\n    \"a\": 1,\n    \"c\": 3\n}\n");
    EXPECT_EQ(remove(text, "a"), "{\n    \"b\": 2,\n    \"c\": 3\n}\n");
}

TEST(EditJsoncProperty, RemovingTheLastPropertyTakesTheCommaBeforeIt) {
    EXPECT_EQ(remove("{\n    \"a\": 1,\n    \"b\": 2\n}\n", "b"), "{\n    \"a\": 1\n}\n");
}

TEST(EditJsoncProperty, RemovingTheLastPropertyKeepsATrailingCommaStyle) {
    EXPECT_EQ(remove("{\n    \"a\": 1,\n    \"b\": 2,\n}\n", "b"), "{\n    \"a\": 1,\n}\n");
}

TEST(EditJsoncProperty, ACommentAtTheEndOfTheLineGoesWithTheProperty) {
    EXPECT_EQ(remove("{\n    // about a\n    \"a\": 1, // one\n    \"b\": 2\n}\n", "a"),
              "{\n    // about a\n    \"b\": 2\n}\n");
}

TEST(EditJsoncProperty, RemovingTheOnlyPropertyLeavesAnEmptyObject) {
    EXPECT_EQ(remove("{\n    \"a\": 1\n}\n", "a"), "{\n}\n");
    EXPECT_EQ(remove(R"j({"a": 1})j", "a"), "{}");
}

TEST(EditJsoncProperty, RemovesFromAnObjectOnOneLine) {
    const std::string text = R"j({"a": 1, "b": 2, "c": 3})j";
    EXPECT_EQ(remove(text, "b"), R"j({"a": 1, "c": 3})j");
    EXPECT_EQ(remove(text, "a"), R"j({"b": 2, "c": 3})j");
    EXPECT_EQ(remove(text, "c"), R"j({"a": 1, "b": 2})j");
}

TEST(EditJsoncProperty, RemovesFromANestedObject) {
    EXPECT_EQ(remove(R"j({"editor": {"fontSize": 14, "tabSize": 2}})j", "editor.fontSize"),
              R"j({"editor": {"tabSize": 2}})j");
    EXPECT_EQ(
        remove(
            "{\n    \"editor\": {\n        \"fontSize\": 14,\n        \"tabSize\": 2\n    }\n}\n",
            "editor.tabSize"),
        "{\n    \"editor\": {\n        \"fontSize\": 14\n    }\n}\n");
}

TEST(EditJsoncProperty, RemovesEverySpellingOfTheKey) {
    EXPECT_EQ(remove(R"j({"editor": {"fontSize": 1}, "editor.fontSize": 2})j", "editor.fontSize"),
              R"j({"editor": {}})j");
    EXPECT_EQ(remove(R"j({"a": 1, "a": 2})j", "a"), "{}");
}

TEST(EditJsoncProperty, RemovingAKeyThatIsNotThereChangesNothing) {
    const std::string text = "{\n    // c\n    \"a\": 1\n}\n";
    EXPECT_EQ(remove(text, "b"), text);
    EXPECT_EQ(remove(text, "a.b"), text);
    EXPECT_EQ(remove("", "a"), "");
    EXPECT_EQ(remove("// only a comment\n", "a"), "// only a comment\n");
}

// --- text that is not a settings file ---

TEST(EditJsoncProperty, ASyntaxErrorFailsWithItsPosition) {
    for (const char* text :
         {"{\n    \"a\": 1,\n    \"b\": \n}", "{\"a\" 1}", "{\"a\": 1", "{\"a\": tru}"}) {
        const auto result = edit_jsonc_property(text, "a", 2);
        ASSERT_FALSE(result.has_value()) << text;
        EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
        EXPECT_EQ(result.error().message, anvil::core::parse_jsonc(text).error().message) << text;
        EXPECT_TRUE(result.error().message.starts_with("line ")) << result.error().message;
    }
}

TEST(EditJsoncProperty, ATopLevelThatIsNotAnObjectFails) {
    for (const char* text : {"[1, 2]", "5", "\"text\"", "null"}) {
        const auto result = edit_jsonc_property(text, "a", 2);
        ASSERT_FALSE(result.has_value()) << text;
        EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
        EXPECT_NE(result.error().message.find("object"), std::string::npos);
    }
}

TEST(EditJsoncProperty, ASyntaxErrorFailsAlsoWhenRemoving) {
    EXPECT_FALSE(edit_jsonc_property("{\"a\": ", "a", std::nullopt).has_value());
}

} // namespace
