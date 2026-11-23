#include "anvil/core/log.hpp"
#include "anvil/ui/resources.hpp"
#include "anvil/ui/theme_store.hpp"

#include <QApplication>
#include <QSignalSpy>
#include <fstream>
#include <gtest/gtest.h>
#include <set>
#include <unistd.h>

using namespace anvil;
using anvil::ui::ThemeStore;
using anvil::ui::WorkbenchColors;

namespace {

class ThemeStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        scratch_ = std::filesystem::temp_directory_path() /
                   ("anvil-theme-store-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(scratch_);
        logger_ = std::make_shared<core::RecordingLogger>();
    }
    void TearDown() override {
        store_.reset();
        std::filesystem::remove_all(scratch_);
    }

    void open_store(const std::filesystem::path& directory) {
        store_ = std::make_unique<ThemeStore>(colors_, directory, logger_);
        failures_ = std::make_unique<QSignalSpy>(store_.get(), &ThemeStore::load_failed);
        changes_ = std::make_unique<QSignalSpy>(store_.get(), &ThemeStore::changed);
    }
    void open_shipped() {
        open_store(ui::themes_directory());
    }
    void write(const std::string& name, const std::string& text) {
        std::ofstream(scratch_ / name) << text;
    }

    WorkbenchColors colors_{QApplication::palette()};
    std::shared_ptr<core::RecordingLogger> logger_;
    std::unique_ptr<ThemeStore> store_;
    std::unique_ptr<QSignalSpy> failures_;
    std::unique_ptr<QSignalSpy> changes_;
    std::filesystem::path scratch_;
};

} // namespace

TEST_F(ThemeStoreTest, ListsTheThemesThatShipWithTheEditor) {
    open_shipped();
    ASSERT_EQ(store_->themes().size(), 10U);
    std::map<std::string, core::ThemeKind> kinds;
    for (const auto& theme : store_->themes()) {
        kinds[theme.id] = theme.kind;
        EXPECT_TRUE(std::filesystem::exists(theme.file)) << theme.id;
    }
    EXPECT_EQ(kinds.at("Dark 2026"), core::ThemeKind::dark);
    EXPECT_EQ(kinds.at("Light 2026"), core::ThemeKind::light);
    EXPECT_EQ(kinds.at("Default High Contrast"), core::ThemeKind::high_contrast_dark);
    EXPECT_EQ(kinds.at("Default High Contrast Light"), core::ThemeKind::high_contrast_light);
    EXPECT_TRUE(store_->current().empty());
    EXPECT_EQ(store_->color_theme(), nullptr);
}

TEST_F(ThemeStoreTest, SelectingAThemeGivesItsColorsToTheWorkbenchKeepingTheAlpha) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark 2026"));
    EXPECT_EQ(store_->current(), "Dark 2026");
    EXPECT_EQ(colors_.color("editor.background"), QColor("#121314"));
    EXPECT_EQ(colors_.color("editor.foreground"), QColor("#BBBEBF"));
    EXPECT_TRUE(colors_.is_themed("statusBar.background"));
    // #276782dd: the selection of the theme is translucent.
    EXPECT_EQ(colors_.color("editor.selectionBackground"), QColor(0x27, 0x67, 0x82, 0xDD));
    EXPECT_EQ(colors_.color("editor.selectionBackground").alpha(), 0xDD);
    ASSERT_NE(store_->color_theme(), nullptr);
    EXPECT_EQ(store_->color_theme()->name(), "2026 Dark");
    EXPECT_FALSE(store_->color_theme()->token_colors().empty());
}

TEST_F(ThemeStoreTest, ATokenThemeRulesAreAvailableToTheHighlighter) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark Modern"));
    EXPECT_FALSE(store_->color_theme()->token_colors().empty());
}

TEST_F(ThemeStoreTest, AThemeIsFoundByItsIdAndOtherwiseByItsLabel) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark High Contrast"));
    EXPECT_EQ(store_->current(), "Default High Contrast");
    EXPECT_EQ(colors_.color("editor.background"), QColor("#000000"));
    ASSERT_TRUE(store_->select("Default High Contrast Light"));
    EXPECT_EQ(store_->current(), "Default High Contrast Light");
}

TEST_F(ThemeStoreTest, SwitchingThemesChangesTheColorsOnceAndSelectingTheSameOneDoesNothing) {
    open_shipped();
    QSignalSpy colors_changed(&colors_, &WorkbenchColors::changed);
    ASSERT_TRUE(store_->select("Dark 2026"));
    ASSERT_EQ(changes_->count(), 1);
    ASSERT_EQ(colors_changed.count(), 1);

    ASSERT_TRUE(store_->select("Light 2026"));
    EXPECT_EQ(changes_->count(), 2);
    EXPECT_EQ(colors_changed.count(), 2);
    EXPECT_EQ(colors_.color("editor.background"), QColor("#FFFFFF"));

    ASSERT_TRUE(store_->select("Light 2026"));
    EXPECT_EQ(changes_->count(), 2);
    EXPECT_EQ(colors_changed.count(), 2);
    EXPECT_EQ(failures_->count(), 0);
}

TEST_F(ThemeStoreTest, AnUnknownNameKeepsTheThemeAndIsReportedOnceAndLogged) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark 2026"));

    EXPECT_FALSE(store_->select("Solarized"));
    EXPECT_EQ(store_->current(), "Dark 2026");
    EXPECT_EQ(colors_.color("editor.background"), QColor("#121314"));
    ASSERT_EQ(failures_->count(), 1);
    EXPECT_TRUE(failures_->at(0).at(0).toString().contains("Solarized"));
    EXPECT_TRUE(logger_->contains(core::LogLevel::error, "Solarized", "theme"));

    EXPECT_FALSE(store_->select("Solarized"));
    EXPECT_EQ(failures_->count(), 1);
    EXPECT_FALSE(store_->select("Monokai"));
    EXPECT_EQ(failures_->count(), 2);

    ASSERT_TRUE(store_->select("Light 2026"));
    EXPECT_FALSE(store_->select("Monokai"));
    EXPECT_EQ(failures_->count(), 3);
}

TEST_F(ThemeStoreTest, WhenNoThemeIsAppliedYetTheFallbackIsUsedQuietly) {
    open_shipped();
    EXPECT_FALSE(store_->select("Solarized", "Dark 2026"));
    EXPECT_EQ(store_->current(), "Dark 2026");
    EXPECT_EQ(colors_.color("editor.background"), QColor("#121314"));
    EXPECT_EQ(failures_->count(), 1);

    EXPECT_FALSE(store_->select("Monokai", "Light 2026"));
    EXPECT_EQ(store_->current(), "Dark 2026");
}

TEST_F(ThemeStoreTest, ABrokenThemeFileKeepsTheThemeAndNamesTheFileAndThePosition) {
    write("themes.json", R"j([
        {"id": "Good", "label": "Good", "uiTheme": "vs", "path": "good.json"},
        {"id": "Broken", "label": "Broken", "uiTheme": "vs-dark", "path": "broken.json"},
        {"id": "Includes", "label": "Includes", "uiTheme": "vs-dark", "path": "includes.json"}
    ])j");
    write("good.json", R"j({"colors": {"editor.background": "#112233"}})j");
    write("broken.json", "{\n  \"colors\": {\"a\": }\n}");
    write("includes.json", R"j({"include": "gone.json"})j");
    open_store(scratch_);

    ASSERT_TRUE(store_->select("Good"));
    EXPECT_EQ(colors_.color("editor.background"), QColor("#112233"));

    EXPECT_FALSE(store_->select("Broken"));
    EXPECT_EQ(store_->current(), "Good");
    EXPECT_EQ(colors_.color("editor.background"), QColor("#112233"));
    ASSERT_EQ(failures_->count(), 1);
    const QString message = failures_->at(0).at(0).toString();
    EXPECT_TRUE(message.contains("Broken"));
    EXPECT_TRUE(message.contains("broken.json"));
    EXPECT_TRUE(message.contains("line 2"));

    EXPECT_FALSE(store_->select("Broken"));
    EXPECT_EQ(failures_->count(), 1);

    EXPECT_FALSE(store_->select("Includes"));
    ASSERT_EQ(failures_->count(), 2);
    EXPECT_TRUE(failures_->at(1).at(0).toString().contains("gone.json"));
}

TEST_F(ThemeStoreTest, AnInvalidColorIsLoggedAndTheThemeStillApplies) {
    write("themes.json", R"j([{"id": "T", "label": "T", "uiTheme": "vs", "path": "t.json"}])j");
    write("t.json",
          R"j({"colors": {"editor.background": "#112233", "editor.foreground": "nope"}})j");
    open_store(scratch_);
    ASSERT_TRUE(store_->select("T"));
    EXPECT_EQ(colors_.color("editor.background"), QColor("#112233"));
    EXPECT_FALSE(colors_.is_themed("editor.foreground"));
    EXPECT_TRUE(logger_->contains(core::LogLevel::warning, "nope", "theme"));
}

TEST_F(ThemeStoreTest, WithoutAListOfThemesThereAreNoThemesAndTheProblemIsSaid) {
    open_store(scratch_ / "nowhere");
    EXPECT_TRUE(store_->themes().empty());
    EXPECT_TRUE(logger_->contains(core::LogLevel::error, "cannot be read", "theme"));
    EXPECT_FALSE(store_->select("Dark 2026", "Dark 2026"));
    ASSERT_EQ(failures_->count(), 1);
    EXPECT_TRUE(failures_->at(0).at(0).toString().contains("no color themes"));
    EXPECT_TRUE(store_->current().empty());

    write("themes.json", "[ not json");
    open_store(scratch_);
    EXPECT_TRUE(store_->themes().empty());
    EXPECT_TRUE(logger_->contains(core::LogLevel::error, "not valid", "theme"));
}

TEST_F(ThemeStoreTest, EntriesOfTheListThatCannotBeUsedAreSkipped) {
    write("themes.json", R"j([5, {"id": "NoPath"}, {"path": "x.json"},
        {"label": "OnlyLabel", "uiTheme": "vs", "path": "t.json"}])j");
    write("t.json", R"j({"colors": {"editor.background": "#010203"}})j");
    open_store(scratch_);
    ASSERT_EQ(store_->themes().size(), 1U);
    EXPECT_EQ(store_->themes()[0].id, "OnlyLabel");
    EXPECT_TRUE(store_->select("OnlyLabel"));
}

TEST_F(ThemeStoreTest, AThemeThatLeavesKeysUnsetGetsThemFromTheDefaultThemeOfItsKind) {
    open_shipped();
    // Dark+ sets the editor's colors and not the status bar's: Dark 2026 stands in.
    ASSERT_TRUE(store_->select("Dark+"));
    EXPECT_EQ(colors_.color("editor.background"), QColor("#1E1E1E"));
    EXPECT_EQ(colors_.color("statusBar.background"), QColor("#191A1B"));
    ASSERT_TRUE(store_->select("Light+"));
    EXPECT_EQ(colors_.color("editor.background"), QColor("#FFFFFF"));
    EXPECT_EQ(colors_.color("statusBar.background"), QColor("#FAFAFD"));

    // High contrast themes are dark or light too.
    ASSERT_TRUE(store_->select("Default High Contrast"));
    EXPECT_EQ(colors_.color("editor.background"), QColor("#000000"));
    EXPECT_EQ(colors_.color("sideBar.background"), QColor("#191A1B"));
    ASSERT_TRUE(store_->select("Default High Contrast Light"));
    EXPECT_EQ(colors_.color("sideBar.background"), QColor("#FAFAFD"));

    // A theme's own value wins over the default theme's.
    ASSERT_TRUE(store_->select("Dark Modern"));
    EXPECT_EQ(colors_.color("statusBar.background"), QColor("#181818"));
}

TEST_F(ThemeStoreTest, TheGutterFollowsTheEditorBackgroundOfAThemeThatDoesNotSetIt) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark Modern"));
    EXPECT_EQ(colors_.color("editor.background"), QColor("#1F1F1F"));
    EXPECT_EQ(colors_.color("editorGutter.background"), QColor("#1F1F1F"));
    ASSERT_TRUE(store_->select("Dark 2026"));
    EXPECT_EQ(colors_.color("editorGutter.background"), QColor("#121314"));
}

TEST_F(ThemeStoreTest, ADefaultThemeThatCannotBeLoadedLeavesTheThemesOwnColors) {
    write("themes.json", R"j([
        {"id": "Dark 2026", "label": "Dark 2026", "uiTheme": "vs-dark", "path": "gone.json"},
        {"id": "Mine", "label": "Mine", "uiTheme": "vs-dark", "path": "mine.json"}
    ])j");
    write("mine.json", R"j({"colors": {"editor.background": "#445566"}})j");
    open_store(scratch_);
    ASSERT_TRUE(store_->select("Mine"));
    EXPECT_EQ(colors_.color("editor.background"), QColor("#445566"));
    EXPECT_FALSE(colors_.is_themed("statusBar.background"));
    EXPECT_TRUE(logger_->contains(core::LogLevel::warning, "gone.json", "theme"));
}

// Every color the window's parts and the editor ask for is set by whichever theme is applied, so
// none of them falls back to the palette. A key added to the table that the default themes do not
// set fails this, to be given a default or dropped.
TEST_F(ThemeStoreTest, EveryThemeSetsEveryKeyOfTheTable) {
    open_shipped();
    for (const auto& theme : store_->themes()) {
        ASSERT_TRUE(store_->select(theme.id));
        for (const ui::WorkbenchColorKey& key : WorkbenchColors::known_keys()) {
            if (!key.optional) {
                EXPECT_TRUE(colors_.is_themed(key.key)) << theme.id << " does not set " << key.key;
            }
        }
    }
}

TEST_F(ThemeStoreTest, AKeyWithoutADefaultStaysUnsetWhenTheThemeLeavesItOut) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark 2026"));
    EXPECT_TRUE(colors_.is_themed("statusBar.border"));
    EXPECT_TRUE(colors_.is_themed("tab.activeBorder"));
    ASSERT_TRUE(store_->select("Dark+"));
    EXPECT_FALSE(colors_.is_themed("statusBar.border"));
    EXPECT_FALSE(colors_.is_themed("tab.activeBorder"));
    EXPECT_FALSE(colors_.is_themed("tab.hoverBackground"));
}

TEST_F(ThemeStoreTest,
       AKeyThatTheDefaultThemesLeaveOutHasTheRegistrysDefaultForDarkAndLightThemes) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark+"));
    EXPECT_EQ(colors_.color("toolbar.hoverBackground"), QColor(0x5a, 0x5d, 0x5e, 0x50));
    ASSERT_TRUE(store_->select("Light+"));
    EXPECT_EQ(colors_.color("toolbar.hoverBackground"), QColor(0xb8, 0xb8, 0xb8, 0x50));
    ASSERT_TRUE(store_->select("Dark High Contrast"));
    EXPECT_FALSE(colors_.is_themed("toolbar.hoverBackground"));
    EXPECT_FALSE(colors_.is_themed("editorGroup.border"));

    ASSERT_TRUE(store_->select("Dark+"));
    EXPECT_EQ(colors_.color("editorGroup.border"), QColor("#444444"));
    ASSERT_TRUE(store_->select("Light+"));
    EXPECT_EQ(colors_.color("editorGroup.border"), QColor("#e7e7e7"));
}

TEST_F(ThemeStoreTest, AKeyThatDefaultsToAnotherTakesItsValue) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark 2026"));
    // The 2026 themes set `focusBorder` and not `sash.hoverBorder`, which defaults to it.
    EXPECT_EQ(colors_.color("sash.hoverBorder"), colors_.color("focusBorder"));
    EXPECT_EQ(colors_.color("sash.hoverBorder"), QColor(0x39, 0x94, 0xBC, 0xB3));
}
