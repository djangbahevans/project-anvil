#include "anvil/ui/seti_icons.hpp"

#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

using namespace anvil::ui;
using nlohmann::json;

namespace {

std::filesystem::path theme_directory() {
    return std::filesystem::path(ANVIL_VSCODE_DIR) / "extensions" / "theme-seti" / "icons";
}

std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

const SetiIcon default_icon{{0xE023, 0xFFD4D7D6}, {0xE023, 0xFFBFC2C1}};

} // namespace

TEST(SetiIconsTest, TheTableHasTheLanguagesOfTheThemeOrderedById) {
    const auto table = seti_icon_table();
    ASSERT_EQ(table.size(), 83U);
    for (std::size_t i = 1; i < table.size(); ++i) {
        EXPECT_LT(table[i - 1].language, table[i].language) << table[i].language;
    }
    EXPECT_EQ(seti_default_icon(), default_icon);
}

TEST(SetiIconsTest, ALanguageHasItsGlyphAndItsColorOfEachKindOfTheme) {
    EXPECT_EQ(seti_icon("python"), (SetiIcon{{0xE07B, 0xFF519ABA}, {0xE07B, 0xFF498BA7}}));
    EXPECT_EQ(seti_icon("jsonc"), seti_icon("json"));
    EXPECT_NE(seti_icon("json"), default_icon);
    for (const SetiIconEntry& entry : seti_icon_table()) {
        EXPECT_EQ(seti_icon(entry.language), entry.icon) << entry.language;
    }
}

TEST(SetiIconsTest, ALanguageThatTheThemeDoesNotNameHasTheDefaultIcon) {
    for (const char* id : {"plaintext", "bibtex", "binary", "diff", "", "no-such-language",
                           "Python", "pytho", "zzzz"}) {
        EXPECT_EQ(seti_icon(id), default_icon) << id;
    }
}

TEST(SetiIconsTest, ALanguageWithNoColorOrLightIconOfItsOwnIsDrawnAsVsCodeDrawsIt) {
    // The dark definition of `todo` has no color, and it has no light one.
    EXPECT_EQ(seti_icon("todo"), (SetiIcon{{0xE096, 0xFFD4D7D6}, default_icon.light}));
}

TEST(SetiIconsTest, TheFontLoadsAndHasEveryGlyphOfTheTable) {
    const QString family = seti_font_family();
    ASSERT_FALSE(family.isEmpty());
    QFont font(family);
    font.setPixelSize(20);
    const QFontMetrics metrics(font);
    EXPECT_TRUE(metrics.inFontUcs4(default_icon.dark.code));
    for (const SetiIconEntry& entry : seti_icon_table()) {
        EXPECT_TRUE(metrics.inFontUcs4(entry.icon.dark.code)) << entry.language;
        EXPECT_TRUE(metrics.inFontUcs4(entry.icon.light.code)) << entry.language;
    }
}

TEST(SetiIconsTest, TheFontIsTheOneInTheCheckout) {
    if (!std::filesystem::exists(theme_directory() / "seti.woff")) {
        GTEST_SKIP() << "needs the pinned VS Code checkout (scripts/fetch-vscode.sh)";
    }
    QFile resource(":/anvil/seti/seti.woff");
    ASSERT_TRUE(resource.open(QIODevice::ReadOnly));
    const QByteArray embedded = resource.readAll();
    EXPECT_EQ(std::string(embedded.constData(), static_cast<std::size_t>(embedded.size())),
              read(theme_directory() / "seti.woff"));
}

TEST(SetiIconsTest, TheDarkIconsAndTheLightOnesAreThoseOfTheCheckout) {
    const auto file = theme_directory() / "vs-seti-icon-theme.json";
    if (!std::filesystem::exists(file)) {
        GTEST_SKIP() << "needs the pinned VS Code checkout (scripts/fetch-vscode.sh)";
    }
    const json theme = json::parse(read(file));
    const auto glyph = [&](const std::string& definition, const SetiGlyph& actual,
                           const std::string& what) {
        const json& wanted = theme.at("iconDefinitions").at(definition);
        EXPECT_EQ(actual.code,
                  std::stoul(wanted.at("fontCharacter").get<std::string>().substr(1), nullptr, 16))
            << what;
        if (wanted.contains("fontColor")) {
            const auto color = wanted.at("fontColor").get<std::string>().substr(1);
            EXPECT_EQ(actual.color, 0xFF000000U | std::stoul(color, nullptr, 16)) << what;
        }
    };
    std::size_t rows = 0;
    for (const auto& [language, definition] : theme.at("languageIds").items()) {
        glyph(definition.get<std::string>(), seti_icon(language).dark, language);
        const json& light = theme.at("light").at("languageIds");
        if (light.contains(language)) {
            glyph(light.at(language).get<std::string>(), seti_icon(language).light, language);
        }
        ++rows;
    }
    glyph(theme.at("file").get<std::string>(), seti_default_icon().dark, "default");
    glyph(theme.at("light").at("file").get<std::string>(), seti_default_icon().light, "default");
    // `jsonc` is in the table also where the theme leaves it to `json`.
    EXPECT_EQ(seti_icon_table().size(),
              rows + (theme.at("languageIds").contains("jsonc") ? 0U : 1U));
}
