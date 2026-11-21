#include "anvil/core/editor_settings.hpp"

#include <gtest/gtest.h>

using anvil::ErrorCode;
using anvil::core::EditorDefaults;
using anvil::core::EditorSettings;
using anvil::core::LineEnding;
using anvil::core::Settings;

namespace {

std::expected<EditorSettings, anvil::Error> read(std::string_view text,
                                                 const EditorDefaults& defaults = {}) {
    const auto settings = Settings::parse(text);
    EXPECT_TRUE(settings.has_value());
    return EditorSettings::from_settings(settings ? *settings : Settings(), defaults);
}

TEST(EditorSettings, AbsentKeysTakeTheDefaults) {
    const EditorDefaults defaults{"Mono", 12, LineEnding::crlf};
    const auto result = read("{}", defaults);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->font_family, "Mono");
    EXPECT_EQ(result->font_size, 12);
    EXPECT_EQ(result->tab_size, 4);
    EXPECT_TRUE(result->insert_spaces);
    EXPECT_EQ(result->line_ending, LineEnding::crlf);
}

TEST(EditorSettings, ReadsEveryKey) {
    const auto result = read(R"j({
        "editor.fontFamily": "Fira Code, monospace",
        "editor.fontSize": 18,
        "editor.tabSize": 2,
        "editor.insertSpaces": false,
        "files.eol": "\r\n"
    })j");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->font_family, "Fira Code, monospace");
    EXPECT_EQ(result->font_size, 18);
    EXPECT_EQ(result->tab_size, 2);
    EXPECT_FALSE(result->insert_spaces);
    EXPECT_EQ(result->line_ending, LineEnding::crlf);
}

TEST(EditorSettings, ReadsNestedSpellings) {
    const auto result =
        read(R"j({"editor": {"tabSize": 8, "fontSize": 20}, "files": {"eol": "\n"}})j",
             {"Mono", 14, LineEnding::crlf});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->tab_size, 8);
    EXPECT_EQ(result->font_size, 20);
    EXPECT_EQ(result->line_ending, LineEnding::lf);
}

TEST(EditorSettings, FontSizeIsClampedAndRounded) {
    EXPECT_EQ(read(R"j({"editor.fontSize": 1})j")->font_size, 6);
    EXPECT_EQ(read(R"j({"editor.fontSize": 6})j")->font_size, 6);
    EXPECT_EQ(read(R"j({"editor.fontSize": 100})j")->font_size, 100);
    EXPECT_EQ(read(R"j({"editor.fontSize": 5000})j")->font_size, 100);
    EXPECT_EQ(read(R"j({"editor.fontSize": -3})j")->font_size, 6);
    EXPECT_EQ(read(R"j({"editor.fontSize": 13.6})j")->font_size, 14);
    EXPECT_EQ(read(R"j({"editor.fontSize": 13.4})j")->font_size, 13);
}

TEST(EditorSettings, TabSizeIsClamped) {
    EXPECT_EQ(read(R"j({"editor.tabSize": 0})j")->tab_size, 1);
    EXPECT_EQ(read(R"j({"editor.tabSize": -4})j")->tab_size, 1);
    EXPECT_EQ(read(R"j({"editor.tabSize": 1})j")->tab_size, 1);
    EXPECT_EQ(read(R"j({"editor.tabSize": 1024})j")->tab_size, 1024);
    EXPECT_EQ(read(R"j({"editor.tabSize": 1000000000000})j")->tab_size, 1024);
}

TEST(EditorSettings, AutoLineEndingFollowsThePlatformDefault) {
    EXPECT_EQ(read(R"j({"files.eol": "auto"})j", {"f", 14, LineEnding::lf})->line_ending,
              LineEnding::lf);
    EXPECT_EQ(read(R"j({"files.eol": "auto"})j", {"f", 14, LineEnding::crlf})->line_ending,
              LineEnding::crlf);
}

TEST(EditorSettings, ABlankFontFamilyKeepsTheDefault) {
    EXPECT_EQ(read(R"j({"editor.fontFamily": "  "})j", {"Mono", 14, LineEnding::lf})->font_family,
              "Mono");
}

TEST(EditorSettings, ValuesOfTheWrongTypeFail) {
    for (const auto& [text, key] : std::vector<std::pair<std::string, std::string>>{
             {R"j({"editor.fontFamily": 3})j", "editor.fontFamily"},
             {R"j({"editor.fontSize": "big"})j", "editor.fontSize"},
             {R"j({"editor.tabSize": "4"})j", "editor.tabSize"},
             {R"j({"editor.tabSize": 2.5})j", "editor.tabSize"},
             {R"j({"editor.insertSpaces": "yes"})j", "editor.insertSpaces"},
             {R"j({"files.eol": 1})j", "files.eol"},
             {R"j({"files.eol": "\r"})j", "files.eol"},
         }) {
        const auto result = read(text);
        ASSERT_FALSE(result.has_value()) << text;
        EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
        EXPECT_NE(result.error().message.find(key), std::string::npos) << result.error().message;
    }
}

TEST(EditorSettings, TheErrorNamesEveryOffendingKey) {
    const auto result =
        read(R"j({"editor.fontSize": "x", "editor.insertSpaces": 1, "editor.tabSize": 2})j");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message,
              "editor.fontSize must be a number; editor.insertSpaces must be true or false");
}

TEST(EditorSettings, UnrelatedKeysDoNotMatter) {
    const auto result = read(R"j({"workbench.colorTheme": 5, "editor.rulers": "no"})j");
    EXPECT_TRUE(result.has_value());
}

} // namespace
