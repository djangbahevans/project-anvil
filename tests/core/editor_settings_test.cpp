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

TEST(EditorSettings, TheZoomLevelDefaultsToZeroAndIsClampedToEightEitherWay) {
    EXPECT_EQ(read("{}")->zoom_level, 0);
    EXPECT_EQ(read(R"j({"window.zoomLevel": 3})j")->zoom_level, 3);
    EXPECT_EQ(read(R"j({"window.zoomLevel": -8})j")->zoom_level, -8);
    EXPECT_EQ(read(R"j({"window.zoomLevel": 9})j")->zoom_level, 8);
    EXPECT_EQ(read(R"j({"window.zoomLevel": -100})j")->zoom_level, -8);
    EXPECT_EQ(read(R"j({"window": {"zoomLevel": 2}})j")->zoom_level, 2);
}

TEST(EditorSettings, TheZoomLevelMayHaveDecimals) {
    EXPECT_DOUBLE_EQ(read(R"j({"window.zoomLevel": 1.5})j")->zoom_level, 1.5);
    EXPECT_DOUBLE_EQ(read(R"j({"window.zoomLevel": -0.25})j")->zoom_level, -0.25);
}

TEST(EditorSettings, AZoomLevelThatIsNotANumberIsReportedByKey) {
    for (const char* text : {R"j({"window.zoomLevel": "2"})j", R"j({"window.zoomLevel": true})j",
                             R"j({"window.zoomLevel": null})j"}) {
        const auto result = read(text);
        ASSERT_FALSE(result.has_value()) << text;
        EXPECT_NE(result.error().message.find("window.zoomLevel must be a number"),
                  std::string::npos)
            << result.error().message;
    }
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

TEST(EditorSettings, TheColorThemeDefaultsToVsCodesDefaultAndIsReadFromTheSettings) {
    EXPECT_EQ(read("{}")->color_theme, "Dark 2026");
    EXPECT_EQ(read("{}", {"f", 14, LineEnding::lf, "Light 2026"})->color_theme, "Light 2026");
    EXPECT_EQ(read(R"j({"workbench.colorTheme": "Dark Modern"})j")->color_theme, "Dark Modern");
    EXPECT_EQ(read(R"j({"workbench": {"colorTheme": "Light+"}})j")->color_theme, "Light+");
    EXPECT_EQ(read(R"j({"workbench.colorTheme": " "})j", {"f", 14, LineEnding::lf, "Light 2026"})
                  ->color_theme,
              "Light 2026");
}

TEST(EditorSettings, FocusingTheRecentEditorAfterClosingDefaultsToOnAndIsReadFromTheSettings) {
    EXPECT_TRUE(read("{}")->focus_recent_editor_after_close);
    EXPECT_FALSE(read(R"j({"workbench.editor.focusRecentEditorAfterClose": false})j")
                     ->focus_recent_editor_after_close);
    EXPECT_FALSE(read(R"j({"workbench": {"editor": {"focusRecentEditorAfterClose": false}}})j")
                     ->focus_recent_editor_after_close);
    EXPECT_TRUE(read(R"j({"workbench.editor.focusRecentEditorAfterClose": true})j")
                    ->focus_recent_editor_after_close);
}

TEST(EditorSettings, TheTitleBarStyleDefaultsToCustomAndIsReadFromTheSettings) {
    using anvil::core::TitleBarStyle;
    EXPECT_EQ(read("{}")->title_bar_style, TitleBarStyle::custom);
    EXPECT_EQ(read(R"j({"window.titleBarStyle": "custom"})j")->title_bar_style,
              TitleBarStyle::custom);
    EXPECT_EQ(read(R"j({"window.titleBarStyle": "native"})j")->title_bar_style,
              TitleBarStyle::native);
    EXPECT_EQ(read(R"j({"window": {"titleBarStyle": "native"}})j")->title_bar_style,
              TitleBarStyle::native);
}

TEST(EditorSettings, TheCommandCenterAndTheLayoutControlsAreOnUnlessTheSettingsTurnThemOff) {
    EXPECT_TRUE(read("{}")->command_center);
    EXPECT_TRUE(read("{}")->layout_controls);
    EXPECT_FALSE(read(R"j({"window.commandCenter": false})j")->command_center);
    EXPECT_TRUE(read(R"j({"window.commandCenter": false})j")->layout_controls);
    EXPECT_FALSE(read(R"j({"workbench.layoutControl.enabled": false})j")->layout_controls);
    EXPECT_FALSE(
        read(R"j({"workbench": {"layoutControl": {"enabled": false}}})j")->layout_controls);
    EXPECT_TRUE(read(R"j({"window.commandCenter": true})j")->command_center);
}

TEST(EditorSettings, ThePartsOfTheWindowAreShownAtTheirDefaultPlacesUnlessTheSettingsSayOtherwise) {
    using anvil::core::ActivityBarLocation;
    using anvil::core::SideBarLocation;
    EXPECT_TRUE(read("{}")->status_bar_visible);
    EXPECT_EQ(read("{}")->side_bar_location, SideBarLocation::left);
    EXPECT_EQ(read("{}")->activity_bar_location, ActivityBarLocation::standard);

    EXPECT_FALSE(read(R"j({"workbench.statusBar.visible": false})j")->status_bar_visible);
    EXPECT_FALSE(read(R"j({"workbench": {"statusBar": {"visible": false}}})j")->status_bar_visible);
    EXPECT_EQ(read(R"j({"workbench.sideBar.location": "right"})j")->side_bar_location,
              SideBarLocation::right);
    EXPECT_EQ(read(R"j({"workbench.sideBar.location": "left"})j")->side_bar_location,
              SideBarLocation::left);
    EXPECT_EQ(read(R"j({"workbench.activityBar.location": "hidden"})j")->activity_bar_location,
              ActivityBarLocation::hidden);
    EXPECT_EQ(read(R"j({"workbench.activityBar.location": "top"})j")->activity_bar_location,
              ActivityBarLocation::top);
    EXPECT_EQ(read(R"j({"workbench.activityBar.location": "bottom"})j")->activity_bar_location,
              ActivityBarLocation::bottom);
    EXPECT_EQ(read(R"j({"workbench.activityBar.location": "default"})j")->activity_bar_location,
              ActivityBarLocation::standard);
}

TEST(EditorSettings, ZenModeHidesAndEntersFullScreenAndCentersUnlessToldNotTo) {
    const auto defaults = read("{}");
    EXPECT_TRUE(defaults->zen_full_screen);
    EXPECT_TRUE(defaults->zen_center_layout);
    EXPECT_TRUE(defaults->zen_hide_status_bar);
    EXPECT_TRUE(defaults->zen_hide_activity_bar);

    EXPECT_FALSE(read(R"j({"zenMode.fullScreen": false})j")->zen_full_screen);
    EXPECT_FALSE(read(R"j({"zenMode": {"centerLayout": false}})j")->zen_center_layout);
    EXPECT_FALSE(read(R"j({"zenMode.hideStatusBar": false})j")->zen_hide_status_bar);
    EXPECT_FALSE(read(R"j({"zenMode.hideActivityBar": false})j")->zen_hide_activity_bar);

    const auto only_one = read(R"j({"zenMode.hideStatusBar": false})j");
    EXPECT_TRUE(only_one->zen_full_screen);
    EXPECT_TRUE(only_one->zen_center_layout);
    EXPECT_TRUE(only_one->zen_hide_activity_bar);
}

TEST(EditorSettings, TabsAreMultipleUnlessTheSettingsSayOtherwise) {
    const auto defaults = read("{}");
    EXPECT_EQ(defaults->show_tabs, anvil::core::TabsMode::multiple);
    EXPECT_EQ(defaults->zen_show_tabs, anvil::core::TabsMode::multiple);

    EXPECT_EQ(read(R"j({"workbench.editor.showTabs": "single"})j")->show_tabs,
              anvil::core::TabsMode::single);
    EXPECT_EQ(read(R"j({"workbench.editor.showTabs": "none"})j")->show_tabs,
              anvil::core::TabsMode::none);
    EXPECT_EQ(read(R"j({"workbench": {"editor": {"showTabs": "multiple"}}})j")->show_tabs,
              anvil::core::TabsMode::multiple);
    EXPECT_EQ(read(R"j({"zenMode.showTabs": "none"})j")->zen_show_tabs,
              anvil::core::TabsMode::none);
    EXPECT_EQ(read(R"j({"zenMode.showTabs": "single"})j")->zen_show_tabs,
              anvil::core::TabsMode::single);

    const auto one = read(R"j({"zenMode.showTabs": "none"})j");
    EXPECT_EQ(one->show_tabs, anvil::core::TabsMode::multiple);
    const auto other = read(R"j({"workbench.editor.showTabs": "none"})j");
    EXPECT_EQ(other->zen_show_tabs, anvil::core::TabsMode::multiple);
}

TEST(EditorSettings, ATabsSettingThatIsNotAModeIsReportedByName) {
    for (const char* key : {"workbench.editor.showTabs", "zenMode.showTabs"}) {
        for (const char* value : {R"j("many")j", "true", "1"}) {
            const auto result = read(std::string("{\"") + key + "\": " + value + "}");
            ASSERT_FALSE(result.has_value()) << key << " " << value;
            EXPECT_NE(result.error().message.find(key), std::string::npos) << key;
        }
    }
}

TEST(EditorSettings, CommentsInsertASpaceAndIgnoreEmptyLinesUnlessTheSettingsSayOtherwise) {
    EXPECT_TRUE(read("{}")->comments_insert_space);
    EXPECT_TRUE(read("{}")->comments_ignore_empty_lines);
    EXPECT_FALSE(read(R"j({"editor.comments.insertSpace": false})j")->comments_insert_space);
    EXPECT_TRUE(read(R"j({"editor.comments.insertSpace": false})j")->comments_ignore_empty_lines);
    EXPECT_FALSE(read(R"j({"editor": {"comments": {"ignoreEmptyLines": false}}})j")
                     ->comments_ignore_empty_lines);
    EXPECT_TRUE(
        read(R"j({"editor": {"comments": {"ignoreEmptyLines": false}}})j")->comments_insert_space);
    for (const char* key : {"editor.comments.insertSpace", "editor.comments.ignoreEmptyLines"}) {
        const auto result = read(std::string(R"j({")j") + key + R"j(": "no"})j");
        ASSERT_FALSE(result.has_value()) << key;
        EXPECT_NE(result.error().message.find(key), std::string::npos) << key;
    }
}

TEST(EditorSettings, AZenModeSettingOfTheWrongKindIsReportedByName) {
    for (const char* key : {"zenMode.fullScreen", "zenMode.centerLayout", "zenMode.hideStatusBar",
                            "zenMode.hideActivityBar"}) {
        const auto result = read(std::string(R"j({")j") + key + R"j(": "no"})j");
        ASSERT_FALSE(result.has_value()) << key;
        EXPECT_NE(result.error().message.find(key), std::string::npos) << key;
    }
}

TEST(EditorSettings, APartSettingOfTheWrongKindIsReportedByName) {
    for (const char* json : {R"j({"workbench.statusBar.visible": "no"})j",
                             R"j({"workbench.sideBar.location": "top"})j",
                             R"j({"workbench.activityBar.location": "side"})j"}) {
        const auto result = read(json);
        ASSERT_FALSE(result.has_value()) << json;
        EXPECT_NE(result.error().message.find("workbench."), std::string::npos) << json;
    }
}

TEST(EditorSettings, AutoSaveIsOffWithADelayOfASecondUnlessTheSettingsSayOtherwise) {
    using anvil::core::AutoSave;
    EXPECT_EQ(read("{}")->auto_save, AutoSave::off);
    EXPECT_EQ(read("{}")->auto_save_delay, 1000);
    EXPECT_EQ(read(R"j({"files.autoSave": "off"})j")->auto_save, AutoSave::off);
    EXPECT_EQ(read(R"j({"files.autoSave": "afterDelay"})j")->auto_save, AutoSave::after_delay);
    EXPECT_EQ(read(R"j({"files.autoSave": "onFocusChange"})j")->auto_save,
              AutoSave::on_focus_change);
    EXPECT_EQ(read(R"j({"files": {"autoSave": "onWindowChange"}})j")->auto_save,
              AutoSave::on_window_change);
    EXPECT_EQ(read(R"j({"files.autoSaveDelay": 250})j")->auto_save_delay, 250);
    EXPECT_EQ(read(R"j({"files.autoSaveDelay": 0})j")->auto_save_delay, 0);
}

TEST(EditorSettings, WhitespaceIsRenderedInSelectionsAndControlCharactersAreShownByDefault) {
    using anvil::core::RenderWhitespace;
    EXPECT_EQ(read("{}")->render_whitespace, RenderWhitespace::selection);
    EXPECT_TRUE(read("{}")->render_control_characters);
    EXPECT_EQ(read(R"j({"editor.renderWhitespace": "none"})j")->render_whitespace,
              RenderWhitespace::none);
    EXPECT_EQ(read(R"j({"editor.renderWhitespace": "boundary"})j")->render_whitespace,
              RenderWhitespace::boundary);
    EXPECT_EQ(read(R"j({"editor": {"renderWhitespace": "trailing"}})j")->render_whitespace,
              RenderWhitespace::trailing);
    EXPECT_EQ(read(R"j({"editor.renderWhitespace": "all"})j")->render_whitespace,
              RenderWhitespace::all);
    EXPECT_FALSE(read(R"j({"editor.renderControlCharacters": false})j")->render_control_characters);
}

TEST(EditorSettings,
     TheMultiCursorModifierIsAltAndColumnSelectionIsOffUnlessTheSettingsSayOtherwise) {
    using anvil::core::MultiCursorModifier;
    EXPECT_EQ(read("{}")->multi_cursor_modifier, MultiCursorModifier::alt);
    EXPECT_FALSE(read("{}")->column_selection);
    EXPECT_EQ(read(R"j({"editor.multiCursorModifier": "ctrlCmd"})j")->multi_cursor_modifier,
              MultiCursorModifier::ctrl_cmd);
    EXPECT_EQ(read(R"j({"editor": {"multiCursorModifier": "alt"}})j")->multi_cursor_modifier,
              MultiCursorModifier::alt);
    EXPECT_TRUE(read(R"j({"editor.columnSelection": true})j")->column_selection);
}

TEST(EditorSettings, TheAutoSaveDelayIsClamped) {
    EXPECT_EQ(read(R"j({"files.autoSaveDelay": -5})j")->auto_save_delay, 0);
    EXPECT_EQ(read(R"j({"files.autoSaveDelay": 1e12})j")->auto_save_delay,
              anvil::core::EditorSettings::max_auto_save_delay);
}

TEST(EditorSettings, ValuesOfTheWrongTypeFail) {
    for (const auto& [text, key] : std::vector<std::pair<std::string, std::string>>{
             {R"j({"editor.fontFamily": 3})j", "editor.fontFamily"},
             {R"j({"editor.fontSize": "big"})j", "editor.fontSize"},
             {R"j({"editor.tabSize": "4"})j", "editor.tabSize"},
             {R"j({"editor.tabSize": 2.5})j", "editor.tabSize"},
             {R"j({"editor.insertSpaces": "yes"})j", "editor.insertSpaces"},
             {R"j({"workbench.colorTheme": 5})j", "workbench.colorTheme"},
             {R"j({"workbench.editor.focusRecentEditorAfterClose": "no"})j",
              "workbench.editor.focusRecentEditorAfterClose"},
             {R"j({"files.eol": 1})j", "files.eol"},
             {R"j({"editor.renderWhitespace": true})j", "editor.renderWhitespace"},
             {R"j({"editor.renderWhitespace": "All"})j", "editor.renderWhitespace"},
             {R"j({"editor.renderControlCharacters": "yes"})j", "editor.renderControlCharacters"},
             {R"j({"editor.multiCursorModifier": "ctrl"})j", "editor.multiCursorModifier"},
             {R"j({"editor.multiCursorModifier": 1})j", "editor.multiCursorModifier"},
             {R"j({"editor.columnSelection": "yes"})j", "editor.columnSelection"},
             {R"j({"files.autoSave": true})j", "files.autoSave"},
             {R"j({"files.autoSave": "afterdelay"})j", "files.autoSave"},
             {R"j({"files.autoSaveDelay": "1s"})j", "files.autoSaveDelay"},
             {R"j({"window.titleBarStyle": 1})j", "window.titleBarStyle"},
             {R"j({"window.commandCenter": "no"})j", "window.commandCenter"},
             {R"j({"workbench.layoutControl.enabled": 0})j", "workbench.layoutControl.enabled"},
             {R"j({"window.titleBarStyle": "inherit"})j", "window.titleBarStyle"},
             {R"j({"window.titleBarStyle": "Native"})j", "window.titleBarStyle"},
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
    const auto result = read(R"j({"workbench.iconTheme": 5, "editor.rulers": "no"})j");
    EXPECT_TRUE(result.has_value());
}

} // namespace

TEST(FileAssociationsSetting, ReadsPatternsAndLanguageIds) {
    const auto settings =
        Settings::parse(R"j({"files.associations": {"*.cfg": "ini", "**/x/*.conf": "json"}})j");
    ASSERT_TRUE(settings.has_value());
    const auto read = anvil::core::read_file_associations(*settings);
    EXPECT_TRUE(read.problems.empty());
    EXPECT_EQ(read.associations, (std::vector<anvil::core::FileAssociation>{{"**/x/*.conf", "json"},
                                                                            {"*.cfg", "ini"}}));
}

TEST(FileAssociationsSetting, ReadsTheNestedSpelling) {
    const auto settings = Settings::parse(R"j({"files": {"associations": {"*.cfg": "ini"}}})j");
    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(anvil::core::read_file_associations(*settings).associations,
              (std::vector<anvil::core::FileAssociation>{{"*.cfg", "ini"}}));
}

TEST(FileAssociationsSetting, AbsentOrNullGivesNothingAndNoProblem) {
    for (const char* text : {"{}", R"j({"files.associations": null})j"}) {
        const auto settings = Settings::parse(text);
        ASSERT_TRUE(settings.has_value());
        const auto read = anvil::core::read_file_associations(*settings);
        EXPECT_TRUE(read.associations.empty());
        EXPECT_TRUE(read.problems.empty());
    }
}

TEST(FileAssociationsSetting, AnEntryThatIsNotALanguageIdIsSkippedWithAProblem) {
    const auto settings = Settings::parse(
        R"j({"files.associations": {"*.a": "ini", "*.b": 3, "*.c": "", "*.d": ["x"], "*.e": null}})j");
    ASSERT_TRUE(settings.has_value());
    const auto read = anvil::core::read_file_associations(*settings);
    EXPECT_EQ(read.associations, (std::vector<anvil::core::FileAssociation>{{"*.a", "ini"}}));
    ASSERT_EQ(read.problems.size(), 4U);
    EXPECT_NE(read.problems[0].find("*.b"), std::string::npos) << read.problems[0];
}

TEST(FileAssociationsSetting, ASettingThatIsNotAnObjectIsAProblem) {
    const auto settings = Settings::parse(R"j({"files.associations": ["*.a"]})j");
    ASSERT_TRUE(settings.has_value());
    const auto read = anvil::core::read_file_associations(*settings);
    EXPECT_TRUE(read.associations.empty());
    EXPECT_EQ(read.problems.size(), 1U);
}

TEST(EditorSettings, TheExplorerAsksBeforeDeletingAndUsesTheTrashUnlessTheSettingsSayOtherwise) {
    const auto defaults = read("{}");
    ASSERT_TRUE(defaults.has_value());
    EXPECT_TRUE(defaults->explorer_confirm_delete);
    EXPECT_TRUE(defaults->enable_trash);
    EXPECT_FALSE(read(R"j({"explorer.confirmDelete": false})j")->explorer_confirm_delete);
    EXPECT_FALSE(read(R"j({"explorer": {"confirmDelete": false}})j")->explorer_confirm_delete);
    EXPECT_FALSE(read(R"j({"files.enableTrash": false})j")->enable_trash);
    EXPECT_TRUE(read(R"j({"files.enableTrash": false})j")->explorer_confirm_delete);
}

TEST(EditorSettings, TheExplorerAsksBeforeADragMovesUnlessTheSettingsSayOtherwise) {
    EXPECT_TRUE(read("{}")->explorer_confirm_drag_and_drop);
    EXPECT_FALSE(
        read(R"j({"explorer.confirmDragAndDrop": false})j")->explorer_confirm_drag_and_drop);
    EXPECT_FALSE(
        read(R"j({"explorer": {"confirmDragAndDrop": false}})j")->explorer_confirm_drag_and_drop);
    EXPECT_TRUE(read(R"j({"explorer.confirmDelete": false})j")->explorer_confirm_drag_and_drop);
}

TEST(EditorSettings, AnExplorerSettingOfTheWrongKindIsReportedByName) {
    for (const char* key :
         {"explorer.confirmDelete", "explorer.confirmDragAndDrop", "files.enableTrash"}) {
        const auto result = read(std::string(R"j({")j") + key + R"j(": "no"})j");
        ASSERT_FALSE(result.has_value()) << key;
        EXPECT_NE(result.error().message.find(key), std::string::npos) << key;
    }
}
