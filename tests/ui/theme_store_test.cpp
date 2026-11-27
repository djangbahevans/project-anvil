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

    EXPECT_EQ(colors_.kind(), core::ThemeKind::dark);

    ASSERT_TRUE(store_->select("Light 2026"));
    EXPECT_EQ(changes_->count(), 2);
    EXPECT_EQ(colors_changed.count(), 2);
    EXPECT_EQ(colors_.color("editor.background"), QColor("#FFFFFF"));
    EXPECT_EQ(colors_.kind(), core::ThemeKind::light);

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

TEST_F(ThemeStoreTest, ThemesGiveTheEditorsTheirSyntaxStyles) {
    open_shipped();
    EXPECT_EQ(store_->token_theme(), nullptr);
    const std::string_view keyword[] = {"source.cpp", "keyword.control"};

    ASSERT_TRUE(store_->select("Dark Modern"));
    const auto dark = store_->token_theme();
    ASSERT_NE(dark, nullptr);
    EXPECT_EQ(dark->style_for(keyword).foreground, (core::Rgba{0xC5, 0x86, 0xC0, 0xff}));
    EXPECT_EQ(colors_.token_theme(), dark);

    QSignalSpy colors_changed(&colors_, &WorkbenchColors::changed);
    ASSERT_TRUE(store_->select("Light Modern"));
    EXPECT_EQ(colors_changed.count(), 1);
    EXPECT_NE(store_->token_theme(), dark);
    EXPECT_EQ(store_->token_theme()->style_for(keyword).foreground,
              (core::Rgba{0xAF, 0x00, 0xDB, 0xff}));

    ASSERT_TRUE(store_->select("Light Modern"));
    EXPECT_EQ(colors_changed.count(), 1);
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

TEST_F(ThemeStoreTest, TheShadowOfPopupsIsTheRegistrysForDarkAndLightThemesAndNoneForHighContrast) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark+"));
    EXPECT_EQ(colors_.color("widget.shadow"), QColor(0, 0, 0, 0x5c));
    ASSERT_TRUE(store_->select("Light+"));
    EXPECT_EQ(colors_.color("widget.shadow"), QColor(0, 0, 0, 0x29));
    ASSERT_TRUE(store_->select("Dark High Contrast"));
    EXPECT_EQ(colors_.color("widget.shadow").alpha(), 0);
    ASSERT_TRUE(store_->select("Light High Contrast"));
    EXPECT_EQ(colors_.color("widget.shadow").alpha(), 0);
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
    EXPECT_EQ(colors_.color("editorGroup.dropBackground"), QColor(0x53, 0x59, 0x5d, 0x80));
    ASSERT_TRUE(store_->select("Light+"));
    EXPECT_EQ(colors_.color("editorGroup.border"), QColor("#e7e7e7"));
    EXPECT_EQ(colors_.color("editorGroup.dropBackground"), QColor(0x26, 0x77, 0xcb, 0x2e));
    ASSERT_TRUE(store_->select("Dark High Contrast"));
    EXPECT_FALSE(colors_.is_themed("editorGroup.dropBackground"));
}

TEST_F(ThemeStoreTest, TheSlidersOfTheScrollBarsTakeTheRegistrysColorsUnlessTheThemeSetsThem) {
    open_shipped();
    const auto sliders = [this] {
        return std::vector<QColor>{colors_.color("scrollbarSlider.background"),
                                   colors_.color("scrollbarSlider.hoverBackground"),
                                   colors_.color("scrollbarSlider.activeBackground")};
    };
    const QColor hover(0x64, 0x64, 0x64, 0xb3);
    for (const auto* dark : {"Dark Modern", "Dark+"}) {
        ASSERT_TRUE(store_->select(dark));
        EXPECT_EQ(sliders(), (std::vector<QColor>{QColor(0x79, 0x79, 0x79, 0x66), hover,
                                                  QColor(0xbf, 0xbf, 0xbf, 0x66)}))
            << dark;
        EXPECT_FALSE(colors_.is_themed("contrastBorder"));
        EXPECT_FALSE(colors_.is_themed("scrollbar.background"));
    }
    ASSERT_TRUE(store_->select("Light+"));
    EXPECT_EQ(sliders(), (std::vector<QColor>{QColor(0x64, 0x64, 0x64, 0x66), hover,
                                              QColor(0x00, 0x00, 0x00, 0x99)}));

    ASSERT_TRUE(store_->select("Dark High Contrast"));
    EXPECT_EQ(colors_.color("contrastBorder"), QColor(0x6f, 0xc3, 0xdf));
    EXPECT_EQ(sliders(),
              (std::vector<QColor>{QColor(0x6f, 0xc3, 0xdf, 153), QColor(0x6f, 0xc3, 0xdf, 204),
                                   QColor(0x6f, 0xc3, 0xdf, 255)}));
    ASSERT_TRUE(store_->select("Default High Contrast Light"));
    EXPECT_EQ(colors_.color("contrastBorder"), QColor(0x0f, 0x4a, 0x85));
    EXPECT_EQ(sliders(),
              (std::vector<QColor>{QColor(0x0f, 0x4a, 0x85, 102), QColor(0x0f, 0x4a, 0x85, 204),
                                   QColor(0x0f, 0x4a, 0x85, 255)}));

    ASSERT_TRUE(store_->select("Dark 2026"));
    EXPECT_EQ(sliders(),
              (std::vector<QColor>{QColor(0xa8, 0xa9, 0xaa, 0x85), QColor(0xa8, 0xa9, 0xaa, 0x90),
                                   QColor(0xa8, 0xa9, 0xaa, 0x9c)}));
}

TEST_F(ThemeStoreTest, TheSlidersOfAHighContrastThemeFollowItsContrastBorder) {
    write("contrast.json", R"j({"colors": {"contrastBorder": "#ff0000"}})j");
    open_shipped();
    store_->add_themes({{.id = "red",
                         .label = "Red",
                         .kind = core::ThemeKind::high_contrast_dark,
                         .file = scratch_ / "contrast.json"}});
    ASSERT_TRUE(store_->select("red"));
    EXPECT_EQ(colors_.color("scrollbarSlider.background"), QColor(0xff, 0, 0, 153));
}

TEST_F(ThemeStoreTest, TheShadowOfAScrolledEditorTakesTheRegistrysColorUnlessTheThemeSetsOne) {
    open_shipped();
    for (const auto* dark : {"Dark Modern", "Dark+"}) {
        ASSERT_TRUE(store_->select(dark));
        EXPECT_EQ(colors_.color("scrollbar.shadow"), QColor("#000000")) << dark;
    }
    ASSERT_TRUE(store_->select("Light+"));
    EXPECT_EQ(colors_.color("scrollbar.shadow"), QColor("#dddddd"));
    ASSERT_TRUE(store_->select("Dark 2026"));
    EXPECT_EQ(colors_.color("scrollbar.shadow"), QColor(0x19, 0x1b, 0x1d, 0x4d));
    ASSERT_TRUE(store_->select("Light 2026"));
    EXPECT_EQ(colors_.color("scrollbar.shadow").alpha(), 0);
    for (const auto* contrast : {"Dark High Contrast", "Default High Contrast Light"}) {
        ASSERT_TRUE(store_->select(contrast));
        EXPECT_FALSE(colors_.is_themed("scrollbar.shadow")) << contrast;
    }
}

TEST_F(ThemeStoreTest, AHighContrastThemeHasTheSelectionColorsOfVsCodesRegistry) {
    write("plain.json", R"j({"colors": {}})j");
    write("own.json", R"j({"colors": {"editor.selectionBackground": "#112233",
                                      "editor.selectionForeground": "#445566"}})j");
    open_shipped();
    store_->add_themes({{.id = "plain",
                         .label = "Plain",
                         .kind = core::ThemeKind::high_contrast_dark,
                         .file = scratch_ / "plain.json"},
                        {.id = "own",
                         .label = "Own",
                         .kind = core::ThemeKind::high_contrast_light,
                         .file = scratch_ / "own.json"}});
    const auto selection = [this](const char* theme) {
        EXPECT_TRUE(store_->select(theme)) << theme;
        return std::pair{colors_.color("editor.selectionBackground"),
                         colors_.is_themed("editor.selectionForeground")
                             ? colors_.color("editor.selectionForeground")
                             : QColor()};
    };
    using Colors = std::pair<QColor, QColor>;
    EXPECT_EQ(selection("Dark High Contrast"), (Colors{QColor("#ffffff"), QColor("#000000")}));
    EXPECT_EQ(selection("Light High Contrast"), (Colors{QColor("#0f4a85"), QColor("#ffffff")}));
    EXPECT_EQ(selection("plain"), (Colors{QColor("#f3f518"), QColor("#000000")}));
    EXPECT_EQ(selection("own"), (Colors{QColor("#112233"), QColor("#445566")}));
    EXPECT_FALSE(selection("Dark 2026").second.isValid());
    EXPECT_FALSE(selection("Light 2026").second.isValid());
}

TEST_F(ThemeStoreTest, AnOptionOfAnInputHasVsCodesHoverBackgroundExceptInHighContrast) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark 2026"));
    EXPECT_EQ(colors_.color("inputOption.hoverBackground"), QColor(0x5a, 0x5d, 0x5e, 0x50));
    ASSERT_TRUE(store_->select("Light 2026"));
    EXPECT_EQ(colors_.color("inputOption.hoverBackground"), QColor(0xb8, 0xb8, 0xb8, 0x50));
    ASSERT_TRUE(store_->select("Dark High Contrast"));
    EXPECT_FALSE(colors_.is_themed("inputOption.hoverBackground"));
}

TEST_F(ThemeStoreTest, TheBorderOfTheActivityBarIsTheThemesOrItsContrastBorder) {
    write("contrast.json", R"j({"colors": {"contrastBorder": "#ff0000"}})j");
    write("both.json",
          R"j({"colors": {"contrastBorder": "#ff0000", "activityBar.border": "#00ff00"}})j");
    open_shipped();
    store_->add_themes({{.id = "red",
                         .label = "Red",
                         .kind = core::ThemeKind::dark,
                         .file = scratch_ / "contrast.json"},
                        {.id = "both",
                         .label = "Both",
                         .kind = core::ThemeKind::high_contrast_dark,
                         .file = scratch_ / "both.json"}});

    ASSERT_TRUE(store_->select("Dark 2026"));
    EXPECT_EQ(colors_.color("activityBar.border"), QColor("#2A2B2C"));
    ASSERT_TRUE(store_->select("Dark+"));
    EXPECT_FALSE(colors_.is_themed("activityBar.border"));
    ASSERT_TRUE(store_->select("Dark High Contrast"));
    EXPECT_EQ(colors_.color("activityBar.border"), QColor("#6fc3df"));
    ASSERT_TRUE(store_->select("Default High Contrast Light"));
    EXPECT_EQ(colors_.color("activityBar.border"), QColor("#0f4a85"));
    ASSERT_TRUE(store_->select("red"));
    EXPECT_EQ(colors_.color("activityBar.border"), QColor("#ff0000"));
    ASSERT_TRUE(store_->select("both"));
    EXPECT_EQ(colors_.color("activityBar.border"), QColor("#00ff00"));
}

TEST_F(ThemeStoreTest, AKeyThatDefaultsToAnotherTakesItsValue) {
    open_shipped();
    ASSERT_TRUE(store_->select("Dark 2026"));
    // The 2026 themes set `focusBorder` and not `sash.hoverBorder`, which defaults to it.
    EXPECT_EQ(colors_.color("sash.hoverBorder"), colors_.color("focusBorder"));
    EXPECT_EQ(colors_.color("sash.hoverBorder"), QColor(0x39, 0x94, 0xBC, 0xB3));
}

// --- themes of extensions ---

namespace {

ThemeStore::Entry contributed(std::string id, std::string label, core::ThemeKind kind,
                              std::filesystem::path file) {
    return {.id = std::move(id), .label = std::move(label), .kind = kind, .file = std::move(file)};
}

} // namespace

TEST_F(ThemeStoreTest, ContributedThemesComeAfterTheBuiltInOnesAndAreSelectedByIdOrLabel) {
    write("ext.json", R"j({"colors": {"editor.background": "#fdf6e3"}})j");
    open_shipped();
    store_->add_themes(
        {contributed("", "Solarized", core::ThemeKind::light, scratch_ / "ext.json"),
         contributed("sol-dark", "Solarized Dark", core::ThemeKind::dark, scratch_ / "ext.json")});
    ASSERT_EQ(store_->themes().size(), 12U);
    EXPECT_EQ(store_->themes()[10].id, "Solarized");
    EXPECT_EQ(store_->themes()[10].kind, core::ThemeKind::light);
    EXPECT_EQ(store_->themes()[11].id, "sol-dark");

    ASSERT_TRUE(store_->select("Solarized"));
    EXPECT_EQ(colors_.color("editor.background"), QColor("#fdf6e3"));
    ASSERT_TRUE(store_->select("Solarized Dark"));
    EXPECT_EQ(store_->current(), "sol-dark");
}

TEST_F(ThemeStoreTest, AContributedThemeTakesTheMissingKeysFromTheDefaultThemeOfItsKind) {
    write("ext.json", R"j({"colors": {"editor.background": "#fdf6e3"}})j");
    open_shipped();
    store_->add_themes(
        {contributed("Mine", "Mine", core::ThemeKind::light, scratch_ / "ext.json")});
    ASSERT_TRUE(store_->select("Mine"));
    ASSERT_TRUE(store_->select("Light 2026"));
    const QColor light_foreground = colors_.color("editor.foreground");
    ASSERT_TRUE(store_->select("Mine"));
    EXPECT_EQ(colors_.color("editor.foreground"), light_foreground);
}

TEST_F(ThemeStoreTest, ATwiceListedNameKeepsTheFirstThemeAndLogsTheOther) {
    write("a.json", R"j({"colors": {"editor.background": "#111111"}})j");
    write("b.json", R"j({"colors": {"editor.background": "#222222"}})j");
    open_shipped();
    store_->add_themes(
        {contributed("Dark 2026", "Dark 2026", core::ThemeKind::dark, scratch_ / "a.json"),
         contributed("Twin", "Twin", core::ThemeKind::dark, scratch_ / "a.json"),
         contributed("Twin", "Twin", core::ThemeKind::dark, scratch_ / "b.json")});
    ASSERT_EQ(store_->themes().size(), 11U);
    EXPECT_TRUE(logger_->contains(core::LogLevel::warning, "'Dark 2026'", "theme"));
    EXPECT_TRUE(logger_->contains(core::LogLevel::warning, "'Twin'", "theme"));

    ASSERT_TRUE(store_->select("Twin"));
    EXPECT_EQ(colors_.color("editor.background"), QColor("#111111"));
    ASSERT_TRUE(store_->select("Dark 2026"));
    EXPECT_NE(colors_.color("editor.background"), QColor("#111111"));
}

TEST_F(ThemeStoreTest, AContributedThemeWithAMissingFileIsListedAsUnavailableAndFailsWhenSelected) {
    write("ok.json", R"j({"colors": {"editor.background": "#333333"}})j");
    open_shipped();
    store_->add_themes({contributed("Gone", "Gone", core::ThemeKind::dark, scratch_ / "gone.json"),
                        contributed("Fine", "Fine", core::ThemeKind::dark, scratch_ / "ok.json")});
    ASSERT_EQ(store_->themes().size(), 12U);
    EXPECT_FALSE(store_->themes()[10].available);
    EXPECT_TRUE(store_->themes()[11].available);
    EXPECT_TRUE(logger_->contains(core::LogLevel::warning, "unavailable", "theme"));

    ASSERT_TRUE(store_->select("Fine"));
    EXPECT_FALSE(store_->select("Gone"));
    EXPECT_EQ(store_->current(), "Fine");
    ASSERT_EQ(failures_->count(), 1);
    EXPECT_TRUE(failures_->at(0).at(0).toString().contains("Gone"));
}

TEST_F(ThemeStoreTest, AnInvalidContributedThemeFileFailsAndTheOthersStillWork) {
    write("bad.json", "{\n  \"colors\": {\"a\": }\n}");
    write("ok.json", R"j({"include": "base.json"})j");
    write("base.json", R"j({"colors": {"editor.background": "#444444"}})j");
    open_shipped();
    store_->add_themes(
        {contributed("Bad", "Bad", core::ThemeKind::dark, scratch_ / "bad.json"),
         contributed("Included", "Included", core::ThemeKind::dark, scratch_ / "ok.json")});
    EXPECT_FALSE(store_->select("Bad"));
    EXPECT_EQ(failures_->count(), 1);
    ASSERT_TRUE(store_->select("Included"));
    EXPECT_EQ(colors_.color("editor.background"), QColor("#444444"));
}

TEST_F(ThemeStoreTest, ThePickerListsDarkThenLightThenHighContrastThemesWithTheDefaultFirst) {
    open_shipped();
    const auto groups = store_->picker_groups();
    const auto ids = [](const std::vector<const ThemeStore::Entry*>& group) {
        std::vector<std::string> names;
        for (const ThemeStore::Entry* entry : group) {
            names.push_back(entry->id);
        }
        return names;
    };
    ASSERT_EQ(groups.size(), 3U);
    EXPECT_EQ(ids(groups[0]), (std::vector<std::string>{"Dark 2026", "Visual Studio Dark",
                                                        "Dark Modern", "Dark+"}));
    EXPECT_EQ(ids(groups[1]), (std::vector<std::string>{"Light 2026", "Visual Studio Light",
                                                        "Light Modern", "Light+"}));
    EXPECT_EQ(ids(groups[2]),
              (std::vector<std::string>{"Default High Contrast", "Default High Contrast Light"}));
}

TEST_F(ThemeStoreTest, ThePickerLeavesOutGroupsWithNoThemeAndSortsByLabelIgnoringCase) {
    write("themes.json", R"j([
        {"id": "b", "label": "banana", "uiTheme": "vs", "path": "t.json"},
        {"id": "a", "label": "Cherry", "uiTheme": "vs", "path": "t.json"},
        {"id": "c", "label": "Apple", "uiTheme": "vs", "path": "t.json"}])j");
    write("t.json", "{}");
    open_store(scratch_);
    const auto groups = store_->picker_groups();
    ASSERT_EQ(groups.size(), 1U);
    ASSERT_EQ(groups[0].size(), 3U);
    EXPECT_EQ(groups[0][0]->label, "Apple");
    EXPECT_EQ(groups[0][1]->label, "banana");
    EXPECT_EQ(groups[0][2]->label, "Cherry");
}

TEST_F(ThemeStoreTest, ThePickerOffersAThemeWhoseFileIsMissingAsUnavailable) {
    open_shipped();
    store_->add_themes({{.id = "gone",
                         .label = "Gone",
                         .kind = core::ThemeKind::dark,
                         .file = scratch_ / "gone.json"}});
    const ThemeStore::Entry* gone = nullptr;
    const auto groups = store_->picker_groups();
    for (const ThemeStore::Entry* entry : groups[0]) {
        if (entry->id == "gone") {
            gone = entry;
        }
    }
    ASSERT_NE(gone, nullptr);
    EXPECT_FALSE(gone->available);
}

TEST_F(ThemeStoreTest, TheGroupsOfThePickerOrderByLabelAndNotById) {
    write("themes.json", R"j([{"id": "z", "label": "Alpha", "uiTheme": "vs-dark", "path": "t.json"},
                              {"id": "a", "label": "Omega", "uiTheme": "vs-dark", "path": "t.json"}])j");
    write("t.json", "{}");
    open_store(scratch_);
    const auto groups = store_->picker_groups();
    ASSERT_EQ(groups.size(), 1U);
    EXPECT_EQ(groups[0][0]->label, "Alpha");
    EXPECT_EQ(groups[0][1]->label, "Omega");
}
