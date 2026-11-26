#include "anvil/ui/seti_icons.hpp"

#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

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
        EXPECT_LT(table[i - 1].key, table[i].key) << table[i].key;
    }
    EXPECT_EQ(seti_default_icon(), default_icon);
}

TEST(SetiIconsTest, ALanguageHasItsGlyphAndItsColorOfEachKindOfTheme) {
    EXPECT_EQ(seti_icon("python"), (SetiIcon{{0xE07B, 0xFF519ABA}, {0xE07B, 0xFF498BA7}}));
    EXPECT_EQ(seti_icon("jsonc"), seti_icon("json"));
    EXPECT_NE(seti_icon("json"), default_icon);
    for (const SetiIconEntry& entry : seti_icon_table()) {
        EXPECT_EQ(seti_icon(entry.key), entry.icon) << entry.key;
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
        EXPECT_TRUE(metrics.inFontUcs4(entry.icon.dark.code)) << entry.key;
        EXPECT_TRUE(metrics.inFontUcs4(entry.icon.light.code)) << entry.key;
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

namespace {

// The icon of each of the 4 fields as one value, for messages.
std::string describe(const SetiGlyph& glyph) {
    std::ostringstream text;
    text << std::hex << static_cast<unsigned>(glyph.code) << "/" << glyph.color;
    return text.str();
}

// A model of the style sheet that VS Code makes of the theme (`fileIconThemeData.ts`) and of how a
// browser applies it to a file row: the rules that match, each property from the most specific one
// that has it, and among equally specific ones from the later one. A definition is one rule, whose
// place in the sheet is where it was first named.
class StyleSheet {
public:
    explicit StyleSheet(const nlohmann::ordered_json& theme) : theme_(theme) {
        for (const auto* section : {&theme_, &theme_.at("light")}) {
            note(section->at("file").get<std::string>());
            for (const char* kind : {"languageIds", "fileExtensions", "fileNames"}) {
                for (const auto& [key, definition] : section->at(kind).items()) {
                    note(definition.template get<std::string>());
                }
            }
        }
    }

    SetiGlyph glyph(const std::string& name, const std::string& language, bool light) const {
        std::pair<int, std::size_t> code_rank{-1, 0};
        std::pair<int, std::size_t> color_rank{-1, 0};
        SetiGlyph result;
        const auto apply = [&](const std::string& definition, int specificity) {
            const auto& wanted = theme_.at("iconDefinitions").at(definition);
            const std::pair<int, std::size_t> rank{specificity, order_.at(definition)};
            if (rank >= code_rank) {
                code_rank = rank;
                result.code = static_cast<char16_t>(std::stoul(
                    wanted.at("fontCharacter").template get<std::string>().substr(1), nullptr, 16));
            }
            if (wanted.contains("fontColor") && rank >= color_rank) {
                color_rank = rank;
                result.color =
                    0xFF000000U |
                    static_cast<QRgb>(std::stoul(
                        wanted.at("fontColor").template get<std::string>().substr(1), nullptr, 16));
            }
        };
        for (int section = 0; section < (light ? 2 : 1); ++section) {
            const auto& rules = section == 0 ? theme_ : theme_.at("light");
            // The light rules have the class of a light theme in front.
            const int more = section;
            apply(rules.at("file").template get<std::string>(), 2 + more);
            const auto& languages = rules.at("languageIds");
            const std::string asked =
                language == "jsonc" && !languages.contains("jsonc") ? "json" : language;
            if (languages.contains(asked)) {
                apply(languages.at(asked).template get<std::string>(), 3 + more);
            }
            for (auto dot = name.find('.'); dot != std::string::npos;
                 dot = name.find('.', dot + 1)) {
                const std::string extension = name.substr(dot + 1);
                if (rules.at("fileExtensions").contains(extension)) {
                    apply(rules.at("fileExtensions").at(extension).template get<std::string>(),
                          3 + segments(extension) + more);
                }
            }
            if (rules.at("fileNames").contains(name)) {
                apply(rules.at("fileNames").at(name).template get<std::string>(),
                      4 + segments(name) + more);
            }
        }
        return result;
    }

private:
    static int segments(const std::string& text) {
        return 1 + static_cast<int>(std::ranges::count(text, '.'));
    }
    void note(const std::string& definition) {
        order_.try_emplace(definition, order_.size());
    }

    const nlohmann::ordered_json& theme_;
    std::map<std::string, std::size_t> order_;
};

} // namespace

TEST(SetiFileIconsTest, TheNameWinsThenTheLongestExtensionThenTheLanguage) {
    const SetiIcon readme{{0xE04D, 0xFF519ABA}, {0xE04D, 0xFF498BA7}};
    const SetiIcon spec{{0xE099, 0xFFE37933}, {0xE099, 0xFFCC6D2E}};
    EXPECT_EQ(seti_file_icon("readme", "python"), readme);
    // A compound extension is more than its last part, and more than the language.
    EXPECT_EQ(seti_file_icon("view.spec.ts", "typescript"), spec);
    EXPECT_NE(seti_file_icon("view.spec.ts", "typescript"), seti_icon("typescript"));
    // Names are compared in lower case.
    EXPECT_EQ(seti_file_icon("README", ""), readme);
    EXPECT_EQ(seti_file_icon("VIEW.Spec.TS", "typescript"), spec);
    // The language alone.
    EXPECT_EQ(seti_file_icon("main.zzz", "python"), seti_icon("python"));
    EXPECT_EQ(seti_file_icon("script", "python"), seti_icon("python"));
    // None of these.
    EXPECT_EQ(seti_file_icon("main.zzz", "plaintext"), seti_default_icon());
    EXPECT_EQ(seti_file_icon("main.zzz", ""), seti_default_icon());
    EXPECT_EQ(seti_file_icon("", ""), seti_default_icon());
}

TEST(SetiFileIconsTest, ANameThatDecidesTheIconNeedsNoLanguage) {
    EXPECT_EQ(seti_icon_by_name("ReadMe"), seti_file_icon("readme", ""));
    EXPECT_EQ(seti_icon_by_name("view.spec.ts"), seti_file_icon("view.spec.ts", "typescript"));
    EXPECT_EQ(seti_icon_by_name("main.zzz"), std::nullopt);
    EXPECT_EQ(seti_icon_by_name(""), std::nullopt);
    // The name gives the glyph, and the color comes from what else the file is.
    EXPECT_EQ(seti_icon_by_name("todo"), std::nullopt);
}

TEST(SetiFileIconsTest, AGlyphWithoutAColorTakesThatOfTheNextRule) {
    EXPECT_EQ(seti_file_icon("todo", "plaintext"),
              (SetiIcon{{0xE096, 0xFFD4D7D6}, {0xE096, 0xFFBFC2C1}}));
    EXPECT_EQ(seti_file_icon("TODO.md", "markdown"),
              (SetiIcon{{0xE096, seti_icon("markdown").dark.color},
                        {0xE096, seti_icon("markdown").light.color}}));
}

TEST(SetiFileIconsTest, TheTablesOfNamesAndExtensionsAreOrderedAndInLowerCase) {
    ASSERT_EQ(seti_file_name_table().size(), 101U);
    ASSERT_EQ(seti_file_extension_table().size(), 238U);
    for (const auto table : {seti_file_name_table(), seti_file_extension_table()}) {
        for (std::size_t i = 0; i < table.size(); ++i) {
            if (i > 0) {
                EXPECT_LT(table[i - 1].key, table[i].key) << table[i].key;
            }
            for (const char c : table[i].key) {
                EXPECT_FALSE(c >= 'A' && c <= 'Z') << table[i].key;
            }
        }
    }
}

TEST(SetiFileIconsTest, EveryFileHasTheIconThatTheStyleSheetOfVsCodeGivesIt) {
    const auto file = theme_directory() / "vs-seti-icon-theme.json";
    if (!std::filesystem::exists(file)) {
        GTEST_SKIP() << "needs the pinned VS Code checkout (scripts/fetch-vscode.sh)";
    }
    const auto theme = nlohmann::ordered_json::parse(read(file));
    const StyleSheet sheet(theme);
    std::size_t cases = 0;
    std::size_t wrong = 0;
    const auto check = [&](const std::string& name, const std::string& language) {
        const SetiIcon icon = seti_file_icon(name, language);
        for (const bool light : {false, true}) {
            const SetiGlyph wanted = sheet.glyph(name, language, light);
            const SetiGlyph actual = light ? icon.light : icon.dark;
            ++cases;
            if (!(actual == wanted) && ++wrong <= 10) {
                ADD_FAILURE() << name << " (" << language << ", " << (light ? "light" : "dark")
                              << "): " << describe(actual) << " for " << describe(wanted);
            }
        }
    };
    const std::vector<std::string> languages{"", "markdown", "python", "todo", "jsonc"};
    for (const auto& [name, definition] : theme.at("fileNames").items()) {
        for (const std::string& language : languages) {
            check(name, language);
        }
    }
    for (const auto& [extension, definition] : theme.at("fileExtensions").items()) {
        for (const std::string& language : languages) {
            check("file." + extension, language);
        }
    }
    for (const auto& [language, definition] : theme.at("languageIds").items()) {
        check("zzz.qqq", language);
    }
    EXPECT_EQ(wrong, 0U);
    EXPECT_GT(cases, 3000U);
}
