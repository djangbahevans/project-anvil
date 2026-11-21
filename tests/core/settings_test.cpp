#include "anvil/core/settings.hpp"

#include <gtest/gtest.h>

using anvil::ErrorCode;
using anvil::core::Settings;
using nlohmann::json;

namespace {

Settings parse(std::string_view text) {
    auto settings = Settings::parse(text);
    EXPECT_TRUE(settings.has_value()) << (settings ? "" : settings.error().message);
    return settings ? *settings : Settings();
}

TEST(Settings, FindsFlatDottedKeys) {
    const Settings settings = parse(R"j({"editor.fontSize": 16})j");
    EXPECT_EQ(settings.get<std::int64_t>("editor.fontSize"), 16);
    EXPECT_TRUE(settings.contains("editor.fontSize"));
}

TEST(Settings, FindsTheSameKeyNested) {
    const Settings settings = parse(R"j({"editor": {"fontSize": 16, "deep": {"x": true}}})j");
    EXPECT_EQ(settings.get<std::int64_t>("editor.fontSize"), 16);
    EXPECT_EQ(settings.get<bool>("editor.deep.x"), true);
}

TEST(Settings, FindsMixedFlatAndNestedSpellings) {
    const Settings settings = parse(R"j({"a.b": {"c": 1}, "a": {"b.d": 2}})j");
    EXPECT_EQ(settings.get<std::int64_t>("a.b.c"), 1);
    EXPECT_EQ(settings.get<std::int64_t>("a.b.d"), 2);
}

TEST(Settings, AFlatKeyWinsOverTheNestedSpelling) {
    const Settings settings = parse(R"j({"editor": {"fontSize": 1}, "editor.fontSize": 2})j");
    EXPECT_EQ(settings.get<std::int64_t>("editor.fontSize"), 2);
}

TEST(Settings, MissingKeysAreAbsent) {
    const Settings settings = parse(R"j({"editor": {"fontSize": 16}, "x": 1})j");
    EXPECT_EQ(settings.find("editor.tabSize"), nullptr);
    EXPECT_EQ(settings.find("editor.fontSize.deeper"), nullptr);
    EXPECT_EQ(settings.find("x.y"), nullptr);
    EXPECT_FALSE(settings.contains("nothing"));
    EXPECT_EQ(settings.get<bool>("nothing"), std::nullopt);
}

TEST(Settings, GettersIgnoreValuesOfAnotherType) {
    const Settings settings = parse(
        R"j({"s": "text", "n": 3, "f": 2.5, "b": true, "big": 18446744073709551615, "neg": -2})j");
    EXPECT_EQ(settings.get<std::string>("s"), "text");
    EXPECT_EQ(settings.get<std::string>("n"), std::nullopt);
    EXPECT_EQ(settings.get<bool>("n"), std::nullopt);
    EXPECT_EQ(settings.get<std::int64_t>("s"), std::nullopt);
    EXPECT_EQ(settings.get<std::int64_t>("f"), std::nullopt);
    EXPECT_EQ(settings.get<std::int64_t>("b"), std::nullopt);
    EXPECT_EQ(settings.get<std::int64_t>("big"), std::nullopt);
    EXPECT_EQ(settings.get<std::int64_t>("neg"), -2);
    EXPECT_EQ(settings.get<double>("f"), 2.5);
    EXPECT_EQ(settings.get<double>("n"), 3.0);
    EXPECT_EQ(settings.get<double>("s"), std::nullopt);
    EXPECT_EQ(settings.get<bool>("b"), true);
}

TEST(Settings, GetOrFallsBackForAbsentAndWronglyTypedValues) {
    const Settings settings = parse(R"j({"n": "three", "m": 5})j");
    EXPECT_EQ(settings.get_or<std::int64_t>("n", 9), 9);
    EXPECT_EQ(settings.get_or<std::int64_t>("absent", 9), 9);
    EXPECT_EQ(settings.get_or<std::int64_t>("m", 9), 5);
}

TEST(Settings, KeepsUnknownKeysAsTheyAreWritten) {
    const Settings settings = parse(R"j({
        // a comment
        "workbench.colorTheme": "Solarized",
        "[python]": {"editor.tabSize": 2},
        "unknown": [1, {"a": null}],
    })j");
    EXPECT_EQ(settings.root(), (json{{"workbench.colorTheme", "Solarized"},
                                     {"[python]", {{"editor.tabSize", 2}}},
                                     {"unknown", json::array({1, json{{"a", nullptr}}})}}));
    EXPECT_EQ(settings.get<std::string>("workbench.colorTheme"), "Solarized");
}

TEST(Settings, TextWithoutContentIsEmptySettings) {
    for (const char* text : {"", "  \n", "// nothing here\n"}) {
        const auto settings = Settings::parse(text);
        ASSERT_TRUE(settings.has_value()) << text;
        EXPECT_EQ(settings->root(), json::object());
    }
}

TEST(Settings, TheTopLevelMustBeAnObject) {
    for (const char* text : {"[]", "3", "null", "\"x\""}) {
        const auto settings = Settings::parse(text);
        ASSERT_FALSE(settings.has_value()) << text;
        EXPECT_EQ(settings.error().code, ErrorCode::invalid_argument);
    }
}

TEST(Settings, SyntaxErrorsCarryTheirPosition) {
    const auto settings = Settings::parse("{\n  \"a\": ,\n}");
    ASSERT_FALSE(settings.has_value());
    EXPECT_EQ(settings.error().message, "line 2, column 8: unexpected ',', expected a value");
}

} // namespace
