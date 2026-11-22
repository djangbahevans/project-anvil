#include "anvil/core/token_theme.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

using namespace anvil::core;
using nlohmann::json;

namespace {

constexpr Rgba rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return {r, g, b, a};
}

constexpr Rgba red = rgb(0xFF, 0, 0);
constexpr Rgba green = rgb(0, 0xFF, 0);
constexpr Rgba blue = rgb(0, 0, 0xFF);
constexpr Rgba grey = rgb(0x11, 0x11, 0x11);

// The style of a token whose scopes are the words of `stack`, outermost first.
TokenStyle style(const TokenTheme& theme, const std::string& stack) {
    std::vector<std::string> words;
    std::istringstream in(stack);
    for (std::string word; in >> word;) {
        words.push_back(word);
    }
    std::vector<std::string_view> views(words.begin(), words.end());
    return theme.style_for(views);
}

TokenTheme theme_of(const json& rules) {
    return TokenTheme::from_json(rules);
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

// The cases were recorded from VS Code's own tokenizer by scripts/generate-token-theme-cases.mjs.
TEST(TokenThemeRecordedTest, EveryStackGetsTheStyleVsCodeGivesIt) {
    const json data =
        json::parse(read_text(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/token_theme_cases.json"));
    ASSERT_FALSE(data["sets"].empty());
    std::size_t checked = 0;
    for (const json& set : data["sets"]) {
        const TokenTheme theme = TokenTheme::from_json(set["rules"]);
        const bool default_foreground = set["defaultForeground"];
        const bool default_background = set["defaultBackground"];
        for (const json& c : set["cases"]) {
            std::vector<std::string> words = {"source.t"};
            for (const json& scope : c[0]) {
                words.push_back(scope);
            }
            const std::vector<std::string_view> views(words.begin(), words.end());
            const TokenStyle result = theme.style_for(views);
            std::string name = set["name"].get<std::string>() + " / ";
            for (const std::string& word : words) {
                name += word + " ";
            }

            EXPECT_EQ(result.font_style.value_or(0), c[1].get<int>()) << name;
            const auto check = [&](const std::optional<Rgba>& actual, const json& expected,
                                   bool has_default, const Rgba& vscode_default,
                                   const char* field) {
                if (expected.is_null()) {
                    EXPECT_FALSE(actual.has_value()) << name << field;
                } else if (actual) {
                    EXPECT_EQ(actual, parse_color(expected.get<std::string>())) << name << field;
                } else {
                    // Without a default rule the tokenizer shows black on white and the port
                    // leaves the color to the editor.
                    EXPECT_FALSE(has_default) << name << field << " is unset";
                    EXPECT_EQ(parse_color(expected.get<std::string>()), vscode_default)
                        << name << field;
                }
            };
            check(result.foreground, c[2], default_foreground, rgb(0, 0, 0), " foreground");
            check(result.background, c[3], default_background, rgb(255, 255, 255), " background");
            ++checked;
        }
    }
    EXPECT_GT(checked, 2000U);
}

// The cases below state the rules in words.

TEST(TokenThemeTest, ARuleWithoutAScopeIsTheDefaultStyle) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"settings": {"foreground": "#ff0000", "background": "#00ff00", "fontStyle": "bold"}}
    ])j"));
    EXPECT_EQ(theme.defaults(), (TokenStyle{red, green, FontStyle::bold}));
    EXPECT_EQ(style(theme, "source.js"), theme.defaults());
    EXPECT_EQ(style(theme, ""), theme.defaults());
    EXPECT_EQ(theme.style_for({}), theme.defaults());
}

TEST(TokenThemeTest, WithNoRulesNothingIsSet) {
    const TokenTheme theme = theme_of(json::array());
    EXPECT_EQ(theme.defaults(), TokenStyle{});
    EXPECT_EQ(style(theme, "string"), TokenStyle{});
}

TEST(TokenThemeTest, ASelectorNamesAScopeByWholeSegments) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "entity.name", "settings": {"foreground": "#ff0000"}}
    ])j"));
    EXPECT_EQ(style(theme, "entity.name").foreground, red);
    EXPECT_EQ(style(theme, "entity.name.function").foreground, red);
    EXPECT_FALSE(style(theme, "entity.namex").foreground.has_value());
    EXPECT_FALSE(style(theme, "entity").foreground.has_value());
    EXPECT_FALSE(style(theme, "other.entity.name").foreground.has_value());
}

TEST(TokenThemeTest, AlternativesAndArraysAndStrayCommas) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "a, b ,c", "settings": {"foreground": "#ff0000"}},
        {"scope": ["d", "e"], "settings": {"foreground": "#00ff00"}},
        {"scope": ",,f,,", "settings": {"foreground": "#0000ff"}}
    ])j"));
    for (const char* scope : {"a", "b", "c"}) {
        EXPECT_EQ(style(theme, scope).foreground, red) << scope;
    }
    for (const char* scope : {"d", "e"}) {
        EXPECT_EQ(style(theme, scope).foreground, green) << scope;
    }
    EXPECT_EQ(style(theme, "f").foreground, blue);
    // The commas at the ends do not make empty selectors, which would be default rules.
    EXPECT_EQ(theme.defaults(), TokenStyle{});
}

TEST(TokenThemeTest, SeveralDefaultRulesAreMergedInTheOrderTheyAreSorted) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"settings": {"foreground": "#ff0000"}},
        {"scope": "", "settings": {"background": "#00ff00", "fontStyle": "bold"}},
        {"scope": "a,,b", "settings": {"foreground": "#0000ff"}}
    ])j"));
    EXPECT_EQ(theme.defaults(), (TokenStyle{blue, green, FontStyle::bold}));
}

TEST(TokenThemeTest, AParentMustBeAnAncestorButNeedNotBeNextToTheScope) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "meta.function entity.name", "settings": {"foreground": "#ff0000"}}
    ])j"));
    EXPECT_EQ(style(theme, "meta.function entity.name.function").foreground, red);
    EXPECT_EQ(style(theme, "meta.function string entity.name").foreground, red);
    EXPECT_FALSE(style(theme, "entity.name").foreground.has_value());
    EXPECT_FALSE(style(theme, "entity.name meta.function").foreground.has_value());
    EXPECT_FALSE(style(theme, "meta.class entity.name").foreground.has_value());
}

TEST(TokenThemeTest, TheParentsMustAppearInTheOrderWritten) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "a b c", "settings": {"foreground": "#ff0000"}}
    ])j"));
    EXPECT_EQ(style(theme, "a b c").foreground, red);
    EXPECT_EQ(style(theme, "a x b y c").foreground, red);
    EXPECT_FALSE(style(theme, "b a c").foreground.has_value());
    EXPECT_FALSE(style(theme, "a c").foreground.has_value());
}

TEST(TokenThemeTest, AnAncestorCannotSatisfyTwoWords) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "a a b", "settings": {"foreground": "#ff0000"}}
    ])j"));
    EXPECT_FALSE(style(theme, "a b").foreground.has_value());
    EXPECT_FALSE(style(theme, "a.x b").foreground.has_value());
    EXPECT_EQ(style(theme, "a a b").foreground, red);
    EXPECT_EQ(style(theme, "a x a b").foreground, red);
}

TEST(TokenThemeTest, ARuleWithParentsLeavesWhatItDoesNotSetToTheRuleOfTheSameScope) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "entity.name", "settings": {"foreground": "#ff0000", "fontStyle": "bold"}},
        {"scope": "meta entity.name", "settings": {"background": "#00ff00"}}
    ])j"));
    EXPECT_EQ(style(theme, "meta entity.name"), (TokenStyle{red, green, FontStyle::bold}));
    EXPECT_EQ(style(theme, "entity.name"), (TokenStyle{red, std::nullopt, FontStyle::bold}));
}

TEST(TokenThemeTest, AChildCombinatorNeedsTheVeryNextAncestor) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "a > b", "settings": {"foreground": "#ff0000"}}
    ])j"));
    EXPECT_EQ(style(theme, "a b").foreground, red);
    EXPECT_EQ(style(theme, "x a b").foreground, red);
    EXPECT_FALSE(style(theme, "a x b").foreground.has_value());
}

TEST(TokenThemeTest, TheMoreSegmentsInTheScopeTheMoreSpecificTheRule) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "entity.name.function", "settings": {"foreground": "#ff0000"}},
        {"scope": "entity.name", "settings": {"foreground": "#00ff00"}},
        {"scope": "entity", "settings": {"foreground": "#0000ff"}}
    ])j"));
    EXPECT_EQ(style(theme, "entity.name.function.js").foreground, red);
    EXPECT_EQ(style(theme, "entity.name.type").foreground, green);
    EXPECT_EQ(style(theme, "entity.other").foreground, blue);
}

TEST(TokenThemeTest, ARuleWithMoreAncestorsBeatsOneWithFewerAtTheSameScopeDepth) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "entity.name", "settings": {"foreground": "#ff0000"}},
        {"scope": "meta entity.name", "settings": {"foreground": "#00ff00"}},
        {"scope": "source meta entity.name", "settings": {"foreground": "#0000ff"}}
    ])j"));
    EXPECT_EQ(style(theme, "entity.name").foreground, red);
    EXPECT_EQ(style(theme, "meta entity.name").foreground, green);
    EXPECT_EQ(style(theme, "source meta entity.name").foreground, blue);
    EXPECT_EQ(style(theme, "source x entity.name").foreground, red);
}

TEST(TokenThemeTest, WhenRulesTieTheLaterOneWins) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "string", "settings": {"foreground": "#ff0000"}},
        {"scope": "string", "settings": {"foreground": "#00ff00"}}
    ])j"));
    EXPECT_EQ(style(theme, "string").foreground, green);
}

TEST(TokenThemeTest, ARuleLeavesTheFieldsItDoesNotSetToTheRulesOfShorterScopes) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "entity.name", "settings": {"foreground": "#ff0000", "fontStyle": "italic"}},
        {"scope": "entity.name.function", "settings": {"background": "#00ff00"}}
    ])j"));
    const TokenStyle function = style(theme, "entity.name.function");
    EXPECT_EQ(function.foreground, red);
    EXPECT_EQ(function.background, green);
    EXPECT_EQ(function.font_style, FontStyle::italic);
}

TEST(TokenThemeTest, ATokenKeepsWhatTheEnclosingScopesGaveItWhereTheInnerOnesHaveNoRule) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"settings": {"foreground": "#111111"}},
        {"scope": "string", "settings": {"foreground": "#ff0000", "fontStyle": "bold"}},
        {"scope": "constant.character.escape", "settings": {"foreground": "#00ff00"}}
    ])j"));
    EXPECT_EQ(style(theme, "source.js string.quoted punctuation.definition.string"),
              (TokenStyle{red, std::nullopt, FontStyle::bold}));
    EXPECT_EQ(style(theme, "source.js string.quoted constant.character.escape"),
              (TokenStyle{green, std::nullopt, FontStyle::bold}));
    EXPECT_EQ(style(theme, "source.js other"), (TokenStyle{grey, std::nullopt, std::nullopt}));
}

TEST(TokenThemeTest, AnEmptyFontStyleClearsTheStyleOfEnclosingScopes) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "string", "settings": {"fontStyle": "italic bold underline strikethrough"}},
        {"scope": "string.escape", "settings": {"fontStyle": ""}}
    ])j"));
    EXPECT_EQ(style(theme, "string").font_style, 15);
    EXPECT_EQ(style(theme, "string string.escape").font_style, 0);
    EXPECT_FALSE(style(theme, "comment").font_style.has_value());
}

TEST(TokenThemeTest, FontStyleWordsAreCombinedAndUnknownWordsIgnored) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "a", "settings": {"fontStyle": "bold  italic bogus"}},
        {"scope": "b", "settings": {"fontStyle": "underline"}},
        {"scope": "c", "settings": {"fontStyle": 3}}
    ])j"));
    EXPECT_EQ(style(theme, "a").font_style, FontStyle::bold | FontStyle::italic);
    EXPECT_EQ(style(theme, "b").font_style, FontStyle::underline);
    EXPECT_FALSE(style(theme, "c").font_style.has_value());
}

TEST(TokenThemeTest, ColorsKeepAlphaAndInvalidOnesAreIgnoredWithAWarning) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "a", "settings": {"foreground": "#ff000080", "background": "#0f0"}},
        {"scope": "b", "settings": {"foreground": "red", "background": "#12345"}}
    ])j"));
    EXPECT_EQ(style(theme, "a").foreground, rgb(0xFF, 0, 0, 0x80));
    EXPECT_EQ(style(theme, "a").background, green);
    EXPECT_EQ(style(theme, "b"), TokenStyle{});
    ASSERT_EQ(theme.warnings().size(), 2U);
    EXPECT_NE(theme.warnings()[0].find("rule 1"), std::string::npos);
    EXPECT_NE(theme.warnings()[0].find("red"), std::string::npos);
}

TEST(TokenThemeTest, EntriesThatCannotBeReadAreSkippedWithAWarning) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        5,
        {"scope": "a"},
        {"scope": "b", "settings": null},
        {"scope": ["c", 7], "settings": {"foreground": "#ff0000"}},
        {"scope": "d", "settings": {"foreground": "#00ff00"}}
    ])j"));
    EXPECT_EQ(style(theme, "c").foreground, red);
    EXPECT_EQ(style(theme, "d").foreground, green);
    EXPECT_FALSE(style(theme, "a").foreground.has_value());
    EXPECT_EQ(theme.warnings().size(), 2U);

    const TokenTheme not_an_array = theme_of(json::object());
    EXPECT_EQ(not_an_array.warnings().size(), 1U);
    EXPECT_EQ(style(not_an_array, "a"), TokenStyle{});
}

TEST(TokenThemeTest, AScopeNameIsMatchedAsWrittenNotTrimmedOrLowercased) {
    const TokenTheme theme = theme_of(json::parse(R"j([
        {"scope": "Keyword", "settings": {"foreground": "#ff0000"}}
    ])j"));
    EXPECT_EQ(style(theme, "Keyword").foreground, red);
    EXPECT_FALSE(style(theme, "keyword").foreground.has_value());
}

// --- the themes that ship with VS Code ---

namespace {

// The rules of Dark+, as the theme loader gives them.
TokenTheme dark_plus() {
    const std::filesystem::path directory =
        std::filesystem::path(ANVIL_VSCODE_DIR) / "extensions" / "theme-defaults" / "themes";
    const auto theme = ColorTheme::load(
        directory / "dark_plus.json", ThemeKind::dark,
        [](const std::filesystem::path& path) -> std::expected<std::string, anvil::Error> {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return std::unexpected(anvil::Error{anvil::ErrorCode::io_error, "cannot open"});
            }
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        });
    EXPECT_TRUE(theme.has_value());
    json rules =
        json::array({json{{"settings", {{"foreground", "#D4D4D4"}, {"background", "#1E1E1E"}}}}});
    if (theme) {
        for (const json& rule : theme->token_colors()) {
            rules.push_back(rule);
        }
    }
    return TokenTheme::from_json(rules);
}

} // namespace

TEST(TokenThemeVsCodeTest, DarkPlusStylesTheTokensOfAProgram) {
    if (!std::filesystem::exists(std::filesystem::path(ANVIL_VSCODE_DIR) / "extensions" /
                                 "theme-defaults" / "themes" / "dark_plus.json")) {
        GTEST_SKIP() << "needs the VS Code checkout (scripts/fetch-vscode.sh)";
    }
    const TokenTheme theme = dark_plus();
    EXPECT_TRUE(theme.warnings().empty());
    EXPECT_EQ(theme.defaults().foreground, rgb(0xD4, 0xD4, 0xD4));

    // Function names, comments, strings and control keywords.
    EXPECT_EQ(style(theme, "source.ts meta.function-call entity.name.function").foreground,
              rgb(0xDC, 0xDC, 0xAA));
    EXPECT_EQ(style(theme, "source.ts comment.line.double-slash").foreground,
              rgb(0x6A, 0x99, 0x55));
    EXPECT_EQ(style(theme, "source.ts string.quoted.double punctuation.definition.string.begin")
                  .foreground,
              rgb(0xCE, 0x91, 0x78));
    EXPECT_EQ(style(theme, "source.ts keyword.control.flow").foreground, rgb(0xC5, 0x86, 0xC0));
    // Text no rule names keeps the editor's color.
    EXPECT_EQ(style(theme, "source.ts meta.block").foreground, rgb(0xD4, 0xD4, 0xD4));
}
