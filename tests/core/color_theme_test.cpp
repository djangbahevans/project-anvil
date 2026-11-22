#include "anvil/core/color_theme.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <sstream>

using namespace anvil;
using namespace anvil::core;
using Files = std::map<std::string, std::string>;

namespace {

ColorTheme::Loader loader_for(const Files& files) {
    return [&files](const std::filesystem::path& path) -> std::expected<std::string, Error> {
        const auto found = files.find(path.generic_string());
        if (found == files.end()) {
            return std::unexpected(Error{ErrorCode::io_error, "no such file"});
        }
        return found->second;
    };
}

std::expected<ColorTheme, Error> load(const Files& files, const std::string& path,
                                      ThemeKind kind = ThemeKind::dark) {
    return ColorTheme::load(path, kind, loader_for(files));
}

ColorTheme loaded(const Files& files, const std::string& path) {
    auto theme = load(files, path);
    EXPECT_TRUE(theme.has_value()) << (theme ? "" : theme.error().message);
    return theme ? std::move(*theme) : ColorTheme{};
}

constexpr Rgba rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return {r, g, b, a};
}

} // namespace

TEST(ParseColorTest, ReadsTheFourHexForms) {
    EXPECT_EQ(parse_color("#1f2e3d"), rgb(0x1F, 0x2E, 0x3D));
    EXPECT_EQ(parse_color("#1F2E3D4C"), rgb(0x1F, 0x2E, 0x3D, 0x4C));
    EXPECT_EQ(parse_color("#abc"), rgb(0xAA, 0xBB, 0xCC));
    EXPECT_EQ(parse_color("#ABCD"), rgb(0xAA, 0xBB, 0xCC, 0xDD));
    EXPECT_EQ(parse_color("#000"), rgb(0, 0, 0));
    EXPECT_EQ(parse_color("#ffffff00"), rgb(255, 255, 255, 0));
}

TEST(ParseColorTest, RejectsEverythingElse) {
    for (const char* text :
         {"", "#", "1f2e3d", "#12", "#12345", "#1234567", "#123456789", "#ggg", "#12345g",
          "#+12345", "#-12345", "red", "rgb(1,2,3)", "# 123456", "#1f2e3d ", "default"}) {
        EXPECT_FALSE(parse_color(text).has_value()) << text;
    }
}

TEST(ThemeKindTest, ComesFromTheManifestsUiTheme) {
    EXPECT_EQ(theme_kind_from_ui_theme("vs-dark"), ThemeKind::dark);
    EXPECT_EQ(theme_kind_from_ui_theme("vs"), ThemeKind::light);
    EXPECT_EQ(theme_kind_from_ui_theme("hc-black"), ThemeKind::high_contrast_dark);
    EXPECT_EQ(theme_kind_from_ui_theme("hc-light"), ThemeKind::high_contrast_light);
    EXPECT_EQ(theme_kind_from_ui_theme(""), ThemeKind::dark);
    EXPECT_EQ(theme_kind_from_ui_theme("anything"), ThemeKind::dark);
}

TEST(ColorThemeTest, ReadsTheColorsNameAndKindOfAFile) {
    const Files files = {{"t.json", R"j({
        "name": "My Theme",
        "colors": {"editor.background": "#112233", "editor.foreground": "#aabbccdd"}
    })j"}};
    const auto theme = load(files, "t.json", ThemeKind::light);
    ASSERT_TRUE(theme.has_value()) << theme.error().message;
    EXPECT_EQ(theme->name(), "My Theme");
    EXPECT_EQ(theme->kind(), ThemeKind::light);
    EXPECT_EQ(theme->color("editor.background"), rgb(0x11, 0x22, 0x33));
    EXPECT_EQ(theme->color("editor.foreground"), rgb(0xAA, 0xBB, 0xCC, 0xDD));
    EXPECT_FALSE(theme->color("statusBar.background").has_value());
    EXPECT_TRUE(theme->warnings().empty());
    EXPECT_TRUE(theme->token_colors().empty());
}

TEST(ColorThemeTest, ReadsCommentsAndTrailingCommas) {
    const Files files = {{"t.json", R"j(// a theme
    {
        /* colors */
        "colors": {"a": "#010203", /* last */ },
        "tokenColors": [ {"scope": "x", "settings": {}}, ],
    })j"}};
    const ColorTheme theme = loaded(files, "t.json");
    EXPECT_EQ(theme.color("a"), rgb(1, 2, 3));
    EXPECT_EQ(theme.token_colors().size(), 1U);
}

TEST(ColorThemeTest, KeysAreCaseSensitiveAndTheLastDefinitionWins) {
    const Files files = {
        {"t.json", R"j({"colors": {"Key": "#000001", "key": "#000002", "key": "#000003"}})j"}};
    const ColorTheme theme = loaded(files, "t.json");
    EXPECT_EQ(theme.color("Key"), rgb(0, 0, 1));
    EXPECT_EQ(theme.color("key"), rgb(0, 0, 3));
    EXPECT_EQ(theme.colors().size(), 2U);
}

TEST(ColorThemeTest, TheIncludingFileIsAppliedOnTopOfWhatItIncludes) {
    const Files files = {
        {"themes/base.json", R"j({"name": "Base", "colors": {"a": "#000001", "b": "#000002"}})j"},
        {"themes/top.json",
         R"j({"name": "Top", "include": "./base.json", "colors": {"b": "#0000ff", "c": "#000003"}})j"}};
    const ColorTheme theme = loaded(files, "themes/top.json");
    EXPECT_EQ(theme.name(), "Top");
    EXPECT_EQ(theme.color("a"), rgb(0, 0, 1));
    EXPECT_EQ(theme.color("b"), rgb(0, 0, 255));
    EXPECT_EQ(theme.color("c"), rgb(0, 0, 3));
}

TEST(ColorThemeTest, TheRulesOfIncludedFilesComeFirst) {
    const Files files = {
        {"base.json", R"j({"tokenColors": [{"name": "base1"}, {"name": "base2"}]})j"},
        {"mid.json", R"j({"include": "base.json", "tokenColors": [{"name": "mid"}]})j"},
        {"top.json",
         R"j({"include": "mid.json", "tokenColors": [{"name": "top1"}, {"name": "top2"}]})j"}};
    const ColorTheme theme = loaded(files, "top.json");
    std::vector<std::string> names;
    for (const auto& rule : theme.token_colors()) {
        names.push_back(rule["name"]);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"base1", "base2", "mid", "top1", "top2"}));
}

TEST(ColorThemeTest, TheNameIsThatOfTheFileAskedFor) {
    const Files files = {{"base.json", R"j({"name": "Base"})j"},
                         {"top.json", R"j({"include": "base.json"})j"}};
    EXPECT_EQ(loaded(files, "top.json").name(), "");
    const Files named = {{"base.json", R"j({"name": "Base"})j"},
                         {"top.json", R"j({"name": 5, "include": "base.json"})j"}};
    EXPECT_EQ(loaded(named, "top.json").name(), "");
}

TEST(ColorThemeTest, TheValueDefaultRemovesTheKeyAnIncludedFileSet) {
    const Files files = {{"base.json", R"j({"colors": {"a": "#000001", "b": "#000002"}})j"},
                         {"top.json", R"j({"include": "base.json", "colors": {"a": "default"}})j"}};
    const ColorTheme theme = loaded(files, "top.json");
    EXPECT_FALSE(theme.color("a").has_value());
    EXPECT_EQ(theme.color("b"), rgb(0, 0, 2));
    EXPECT_TRUE(theme.warnings().empty());
}

TEST(ColorThemeTest, AValueThatIsNotAStringIsIgnored) {
    const Files files = {{"base.json", R"j({"colors": {"a": "#000001"}})j"},
                         {"top.json", R"j({"include": "base.json",
                              "colors": {"a": 5, "b": null, "c": true, "d": ["#000000"], "e": {}}})j"}};
    const ColorTheme theme = loaded(files, "top.json");
    EXPECT_EQ(theme.color("a"), rgb(0, 0, 1));
    EXPECT_EQ(theme.colors().size(), 1U);
    EXPECT_TRUE(theme.warnings().empty());
}

TEST(ColorThemeTest, AnInvalidColorIsSkippedWithAWarningThatNamesTheKeyAndTheFile) {
    const Files files = {{"base.json", R"j({"colors": {"a": "#000001"}})j"},
                         {"sub/top.json", R"j({"include": "../base.json",
                              "colors": {"a": "#xyz", "b": "red", "c": "#0000ff"}})j"}};
    const ColorTheme theme = loaded(files, "sub/top.json");
    EXPECT_EQ(theme.color("a"), rgb(0, 0, 1));
    EXPECT_FALSE(theme.color("b").has_value());
    EXPECT_EQ(theme.color("c"), rgb(0, 0, 255));
    ASSERT_EQ(theme.warnings().size(), 2U);
    EXPECT_NE(theme.warnings()[0].find("sub/top.json"), std::string::npos);
    EXPECT_NE(theme.warnings()[0].find("'a'"), std::string::npos);
    EXPECT_NE(theme.warnings()[0].find("#xyz"), std::string::npos);
    EXPECT_NE(theme.warnings()[1].find("'b'"), std::string::npos);
}

TEST(ColorThemeTest, IncludesAreFoundRelativeToTheFileThatNamesThem) {
    const Files files = {{"themes/a/top.json",
                          R"j({"include": "../shared/base.json", "colors": {"top": "#000001"}})j"},
                         {"themes/shared/base.json",
                          R"j({"include": "./deep/more.json", "colors": {"base": "#000002"}})j"},
                         {"themes/shared/deep/more.json", R"j({"colors": {"more": "#000003"}})j"}};
    const ColorTheme theme = loaded(files, "themes/a/top.json");
    EXPECT_EQ(theme.colors().size(), 3U);
    EXPECT_EQ(theme.color("more"), rgb(0, 0, 3));
}

TEST(ColorThemeTest, AnIncludeCycleIsAnErrorThatNamesTheChain) {
    const Files files = {{"a.json", R"j({"include": "b.json"})j"},
                         {"b.json", R"j({"include": "a.json"})j"}};
    const auto theme = load(files, "a.json");
    ASSERT_FALSE(theme.has_value());
    EXPECT_NE(theme.error().message.find("cycle"), std::string::npos);
    EXPECT_NE(theme.error().message.find("a.json -> b.json -> a.json"), std::string::npos);

    const Files itself = {{"a.json", R"j({"include": "./a.json"})j"}};
    EXPECT_FALSE(load(itself, "a.json").has_value());
}

TEST(ColorThemeTest, AMissingFileIsAnErrorThatNamesTheChain) {
    const Files files = {{"a.json", R"j({"include": "b.json"})j"},
                         {"b.json", R"j({"include": "gone.json"})j"}};
    const auto theme = load(files, "a.json");
    ASSERT_FALSE(theme.has_value());
    EXPECT_EQ(theme.error().code, ErrorCode::io_error);
    EXPECT_NE(theme.error().message.find("a.json -> b.json -> gone.json"), std::string::npos);
    EXPECT_NE(theme.error().message.find("no such file"), std::string::npos);

    const auto missing_root = load(files, "nothing.json");
    ASSERT_FALSE(missing_root.has_value());
    EXPECT_NE(missing_root.error().message.find("nothing.json"), std::string::npos);
}

TEST(ColorThemeTest, IncludesNestToADepthOfSixteen) {
    const auto chain = [](int length) {
        Files files;
        for (int i = 0; i < length; ++i) {
            const std::string next = "t" + std::to_string(i + 1) + ".json";
            files["t" + std::to_string(i) + ".json"] =
                i + 1 < length ? R"j({"include": ")j" + next + R"j(", "colors": {"k)j" +
                                     std::to_string(i) + R"j(": "#000000"}})j"
                               : R"j({"colors": {"last": "#000000"}})j";
        }
        return files;
    };
    // t0 includes t1 ... t16: sixteen includes.
    const Files sixteen = chain(17);
    const auto theme = load(sixteen, "t0.json");
    ASSERT_TRUE(theme.has_value()) << theme.error().message;
    EXPECT_TRUE(theme->color("last").has_value());

    const Files seventeen = chain(18);
    const auto too_deep = load(seventeen, "t0.json");
    ASSERT_FALSE(too_deep.has_value());
    EXPECT_EQ(too_deep.error().code, ErrorCode::limit_exceeded);
}

TEST(ColorThemeTest, WhatIsNotSupportedIsAnErrorThatSaysSo) {
    const auto message = [](const std::string& text, const std::string& include_file = "") {
        Files files = {{"t.json", text}};
        if (!include_file.empty()) {
            files["inc.tmTheme"] = "<plist/>";
        }
        const auto theme = load(files, "t.json");
        return theme ? std::string() : theme.error().message;
    };
    EXPECT_NE(message(R"j({"tokenColors": "./theme.tmTheme"})j").find("TextMate"),
              std::string::npos);
    EXPECT_NE(message(R"j({"settings": [{"settings": {"foreground": "#fff"}}]})j").find("legacy"),
              std::string::npos);
    EXPECT_NE(message(R"j({"include": "inc.tmTheme"})j", "x").find("JSON"), std::string::npos);
    EXPECT_NE(message(R"j([1, 2])j").find("JSON object"), std::string::npos);
    EXPECT_NE(message(R"j({"include": 5})j").find("include"), std::string::npos);
    EXPECT_NE(message(R"j({"colors": ["#000000"]})j").find("colors"), std::string::npos);
    EXPECT_NE(message(R"j({"tokenColors": 5})j").find("tokenColors"), std::string::npos);

    Files not_json = {{"t.tmTheme", "<plist/>"}};
    EXPECT_FALSE(load(not_json, "t.tmTheme").has_value());
}

TEST(ColorThemeTest, ASyntaxErrorNamesTheFileAndThePosition) {
    const Files files = {{"base.json", "{\n  \"colors\": {\"a\": }\n}"},
                         {"t.json", R"j({"include": "base.json"})j"}};
    const auto theme = load(files, "t.json");
    ASSERT_FALSE(theme.has_value());
    EXPECT_NE(theme.error().message.find("base.json"), std::string::npos);
    EXPECT_NE(theme.error().message.find("line 2"), std::string::npos);
}

TEST(ColorThemeTest, ANullOrAbsentSectionIsIgnoredAndAnEmptyFileIsNotATheme) {
    const Files files = {{"t.json", R"j({"include": null, "colors": null, "tokenColors": null})j"}};
    const ColorTheme theme = loaded(files, "t.json");
    EXPECT_TRUE(theme.colors().empty());
    EXPECT_TRUE(theme.token_colors().empty());
    EXPECT_FALSE(load({{"e.json", ""}}, "e.json").has_value());
}

TEST(ColorThemeTest, SemanticSettingsAreIgnored) {
    const Files files = {{"t.json", R"j({"semanticHighlighting": true,
                              "semanticTokenColors": {"variable": "#fff"}, "colors": {"a": "#000001"}})j"}};
    const ColorTheme theme = loaded(files, "t.json");
    EXPECT_EQ(theme.colors().size(), 1U);
}

// --- the themes that ship with VS Code ---

namespace {

class VsCodeThemesTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ =
            std::filesystem::path(ANVIL_VSCODE_DIR) / "extensions" / "theme-defaults" / "themes";
        if (!std::filesystem::exists(directory_ / "dark_modern.json")) {
            GTEST_SKIP() << "needs the VS Code checkout (scripts/fetch-vscode.sh)";
        }
    }

    std::expected<ColorTheme, Error> load_theme(const std::string& file, ThemeKind kind) const {
        return ColorTheme::load(
            directory_ / file, kind,
            [](const std::filesystem::path& path) -> std::expected<std::string, Error> {
                std::ifstream in(path, std::ios::binary);
                if (!in) {
                    return std::unexpected(Error{ErrorCode::io_error, "cannot open"});
                }
                std::ostringstream text;
                text << in.rdbuf();
                return text.str();
            });
    }

    std::filesystem::path directory_;
};

} // namespace

TEST_F(VsCodeThemesTest, EveryDefaultThemeLoadsWithoutWarnings) {
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        const auto theme = load_theme(entry.path().filename().string(), ThemeKind::dark);
        ASSERT_TRUE(theme.has_value()) << entry.path().filename() << ": " << theme.error().message;
        EXPECT_TRUE(theme->warnings().empty())
            << entry.path().filename() << ": " << theme->warnings().front();
        EXPECT_FALSE(theme->name().empty()) << entry.path().filename();
        // The light high contrast theme leaves the editor colors to VS Code's defaults.
        if (entry.path().filename() != "hc_light.json") {
            EXPECT_TRUE(theme->color("editor.background").has_value()) << entry.path().filename();
            EXPECT_TRUE(theme->color("editor.foreground").has_value()) << entry.path().filename();
        }
        EXPECT_FALSE(theme->token_colors().empty()) << entry.path().filename();
    }
}

TEST_F(VsCodeThemesTest, TheEditorColorsAreTheOnesInTheFiles) {
    struct Expected {
        const char* file;
        const char* name;
        Rgba background;
        Rgba foreground;
    };
    const Expected expected[] = {
        {"dark_modern.json", "Dark Modern", rgb(0x1F, 0x1F, 0x1F), rgb(0xCC, 0xCC, 0xCC)},
        {"dark_plus.json", "Dark+", rgb(0x1E, 0x1E, 0x1E), rgb(0xD4, 0xD4, 0xD4)},
        {"dark_vs.json", "Dark (Visual Studio)", rgb(0x1E, 0x1E, 0x1E), rgb(0xD4, 0xD4, 0xD4)},
        {"light_modern.json", "Light Modern", rgb(0xFF, 0xFF, 0xFF), rgb(0x3B, 0x3B, 0x3B)},
        {"2026-dark.json", "2026 Dark", rgb(0x12, 0x13, 0x14), rgb(0xBB, 0xBE, 0xBF)},
    };
    for (const Expected& e : expected) {
        const auto theme = load_theme(e.file, ThemeKind::dark);
        ASSERT_TRUE(theme.has_value()) << e.file << ": " << theme.error().message;
        EXPECT_EQ(theme->name(), e.name) << e.file;
        EXPECT_EQ(theme->color("editor.background"), e.background) << e.file;
        EXPECT_EQ(theme->color("editor.foreground"), e.foreground) << e.file;
    }
}

TEST_F(VsCodeThemesTest, AnIncludedThemesColorsShowThroughAndTheRulesKeepTheirOrder) {
    const auto modern = load_theme("dark_modern.json", ThemeKind::dark);
    const auto plus = load_theme("dark_plus.json", ThemeKind::dark);
    const auto vs = load_theme("dark_vs.json", ThemeKind::dark);
    ASSERT_TRUE(modern && plus && vs);

    // Dark Modern keeps what Dark+ and Dark (Visual Studio) define and overrides some of it.
    EXPECT_GT(modern->colors().size(), vs->colors().size());
    EXPECT_NE(modern->color("editor.background"), vs->color("editor.background"));

    // The rules of the three files are the rules of Dark (Visual Studio), then Dark+'s, and Dark
    // Modern adds none.
    EXPECT_EQ(modern->token_colors(), plus->token_colors());
    ASSERT_GT(plus->token_colors().size(), vs->token_colors().size());
    for (std::size_t i = 0; i < vs->token_colors().size(); ++i) {
        EXPECT_EQ(plus->token_colors()[i], vs->token_colors()[i]) << i;
    }
}

TEST_F(VsCodeThemesTest, TheKindIsWhatTheCallerSays) {
    const auto light = load_theme("light_plus.json", ThemeKind::light);
    ASSERT_TRUE(light.has_value());
    EXPECT_EQ(light->kind(), ThemeKind::light);
    const auto hc = load_theme("hc_black.json", ThemeKind::high_contrast_dark);
    ASSERT_TRUE(hc.has_value());
    EXPECT_EQ(hc->kind(), ThemeKind::high_contrast_dark);
}
