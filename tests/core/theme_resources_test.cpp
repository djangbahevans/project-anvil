#include "anvil/core/color_theme.hpp"
#include "anvil/core/editor_settings.hpp"
#include "anvil/core/jsonc.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

using namespace anvil;
using namespace anvil::core;
using nlohmann::json;

namespace {

const std::filesystem::path source_dir = ANVIL_SOURCE_DIR;
const std::filesystem::path themes_dir = source_dir / "resources" / "themes";

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

json shipped_list() {
    return json::parse(read_text(themes_dir / "themes.json"));
}

std::set<std::string> theme_files() {
    std::set<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(themes_dir)) {
        if (entry.path().extension() == ".json" && entry.path().filename() != "themes.json") {
            files.insert(entry.path().filename().string());
        }
    }
    return files;
}

} // namespace

TEST(ThemeResourcesTest, EveryEntryOfTheListHasItsFieldsAndAnExistingFile) {
    const json list = shipped_list();
    ASSERT_TRUE(list.is_array());
    ASSERT_FALSE(list.empty());
    std::set<std::string> ids;
    for (const json& theme : list) {
        for (const char* field : {"id", "label", "uiTheme", "path"}) {
            ASSERT_TRUE(theme.contains(field) && theme[field].is_string() &&
                        !theme[field].get<std::string>().empty())
                << field << " of " << theme.dump();
        }
        EXPECT_TRUE(ids.insert(theme["id"]).second) << "duplicate id " << theme["id"];
        const std::string ui_theme = theme["uiTheme"];
        EXPECT_TRUE(ui_theme == "vs-dark" || ui_theme == "vs" || ui_theme == "hc-black" ||
                    ui_theme == "hc-light")
            << ui_theme;
        EXPECT_TRUE(std::filesystem::exists(themes_dir / theme["path"].get<std::string>()))
            << theme["path"];
    }
}

TEST(ThemeResourcesTest, EveryThemeFileIsListed) {
    std::set<std::string> listed;
    for (const json& theme : shipped_list()) {
        listed.insert(theme["path"]);
    }
    EXPECT_EQ(theme_files(), listed);
}

TEST(ThemeResourcesTest, EveryFileParsesAndEveryIncludeIsAShippedFile) {
    const std::set<std::string> files = theme_files();
    ASSERT_FALSE(files.empty());
    for (const std::string& file : files) {
        const auto parsed = parse_jsonc(read_text(themes_dir / file));
        ASSERT_TRUE(parsed.has_value()) << file << ": " << parsed.error().message;
        if (const auto include = parsed->find("include"); include != parsed->end()) {
            const std::string included =
                std::filesystem::path(include->get<std::string>()).lexically_normal().string();
            EXPECT_TRUE(files.contains(included)) << file << " includes " << included;
        }
    }
}

TEST(ThemeResourcesTest, EveryListedThemeLoadsWithTheKindItsUiThemeSays) {
    const auto loader = [](const std::filesystem::path& path) -> std::expected<std::string, Error> {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot open"});
        }
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    for (const json& entry : shipped_list()) {
        const ThemeKind kind = theme_kind_from_ui_theme(entry["uiTheme"].get<std::string>());
        const auto theme =
            ColorTheme::load(themes_dir / entry["path"].get<std::string>(), kind, loader);
        ASSERT_TRUE(theme.has_value()) << entry["id"] << ": " << theme.error().message;
        EXPECT_TRUE(theme->warnings().empty()) << entry["id"];
        EXPECT_FALSE(theme->token_colors().empty()) << entry["id"];
    }
}

TEST(ThemeResourcesTest, TheDefaultThemeIsOneOfTheShippedOnes) {
    std::set<std::string> ids;
    for (const json& theme : shipped_list()) {
        ids.insert(theme["id"]);
    }
    EXPECT_EQ(EditorDefaults{}.color_theme, "Dark 2026");
    EXPECT_TRUE(ids.contains(EditorDefaults{}.color_theme));
    EXPECT_TRUE(ids.contains("Light 2026"));
}

TEST(ThemeResourcesTest, TheLicenseOfTheThemesIsKeptAndNamedInTheNotices) {
    const std::string license = read_text(themes_dir / "LICENSE-vscode.txt");
    EXPECT_NE(license.find("MIT License"), std::string::npos);
    EXPECT_NE(license.find("Microsoft Corporation"), std::string::npos);

    const std::string notices = read_text(source_dir / "THIRD_PARTY_NOTICES.md");
    EXPECT_NE(notices.find("Visual Studio Code"), std::string::npos);
    EXPECT_NE(notices.find("resources/themes/LICENSE-vscode.txt"), std::string::npos);
    EXPECT_NE(notices.find("Microsoft Corporation"), std::string::npos);
}

// The copies are the checkout's files byte for byte. A newer VS Code that changes a theme fails
// this until `node scripts/sync-themes.mjs` refreshes them.
TEST(ThemeResourcesTest, TheCopiesAreTheFilesOfThePinnedVsCode) {
    const std::filesystem::path extension =
        std::filesystem::path(ANVIL_VSCODE_DIR) / "extensions" / "theme-defaults";
    if (!std::filesystem::exists(extension / "themes" / "dark_modern.json")) {
        GTEST_SKIP() << "needs the VS Code checkout (scripts/fetch-vscode.sh)";
    }
    std::set<std::string> upstream;
    for (const auto& entry : std::filesystem::directory_iterator(extension / "themes")) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        const std::string name = entry.path().filename().string();
        upstream.insert(name);
        EXPECT_EQ(read_text(themes_dir / name), read_text(entry.path())) << name;
    }
    EXPECT_EQ(upstream, theme_files());
    EXPECT_EQ(read_text(themes_dir / "LICENSE-vscode.txt"),
              read_text(std::filesystem::path(ANVIL_VSCODE_DIR) / "LICENSE.txt"));
}
