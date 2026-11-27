#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/language_registry.hpp"
#include "anvil/core/settings.hpp"
#include "anvil/core/text_file.hpp"
#include "anvil/core/wheel_scroll.hpp"

#include <expected>
#include <string>
#include <vector>

namespace anvil::core {

// What the settings default to on a platform. The values are VS Code's per-platform defaults;
// `platform::editor_defaults()` supplies them, so that core holds no per-OS code.
struct EditorDefaults {
    // A comma-separated list of families, with quotes around names that need them.
    std::string font_family = "'Droid Sans Mono', monospace";
    int font_size = 14;
    // The line ending that `files.eol: "auto"` stands for.
    LineEnding auto_line_ending = LineEnding::lf;
    // The color theme when `workbench.colorTheme` is not set: VS Code's default in the pinned
    // release, which is the `id` of one of the themes in resources/themes/themes.json.
    std::string color_theme = "Dark 2026";
    // The distance between lines as a multiple of the font size: VS Code's, which is 1.5 on macOS.
    double line_height_ratio = 1.35;
};

// Whether the window draws its own title bar (`custom`) or has the system's frame (`native`).
enum class TitleBarStyle { custom, native };

// files.autoSave: when a document with a file and unsaved changes is saved without being asked.
enum class AutoSave { off, after_delay, on_focus_change, on_window_change };

// editor.renderWhitespace: which spaces and tabs the editor draws a mark for.
enum class RenderWhitespace { none, boundary, selection, trailing, all };

// editor.wordWrap: where the lines of the editor wrap: nowhere, at the width of the view, at
// editor.wordWrapColumn, or at the smaller of the two.
enum class WordWrap { off, on, word_wrap_column, bounded };

// editor.multiCursorModifier: the key that makes a click add a cursor. `ctrl_cmd` is Ctrl, or Cmd
// on macOS.
enum class MultiCursorModifier { alt, ctrl_cmd };

// window.restoreWindows: what a start opens again. With one window to a process, `all`, `folders`
// and `one` open the folder of the window closed last when nothing is named, `preserve` also when
// files are, and `none` never does.
enum class RestoreWindows { preserve, all, folders, one, none };

// workbench.sideBar.location: the edge of the window the primary side bar is on.
enum class SideBarLocation { left, right };

// workbench.activityBar.location. `top` and `bottom` are valid values that the window shows as
// `standard`, because the activity bar has no place other than beside the side bar yet.
enum class ActivityBarLocation { standard, top, bottom, hidden };

// workbench.editor.showTabs and zenMode.showTabs: every editor of a group is a tab, only the active
// editor is shown as one large tab, or the group has no strip of tabs.
enum class TabsMode { multiple, single, none };

// workbench.editor.untitled.labelFormat: what an untitled document is named after.
enum class UntitledLabel { content, name };

// The editor's settings, read from `Settings` with VS Code's key names, ranges and defaults.
struct EditorSettings {
    static constexpr int min_font_size = 6;
    static constexpr int max_font_size = 100;
    static constexpr int max_tab_size = 1024;
    static constexpr int max_word_wrap_column = 1 << 30;
    static constexpr int max_scroll_beyond_last_column = 1 << 30;
    static constexpr int max_auto_save_delay = 24 * 60 * 60 * 1000;

    // editor.fontFamily
    std::string font_family;
    // editor.fontSize, in pixels
    int font_size = 14;
    // The distance between lines as a multiple of the font size, from EditorDefaults
    double line_height_ratio = 1.35;
    // window.zoomLevel: the level of the window's zoom, from -8 to 8 (see `zoom_factor`); the
    // editor's font is `font_size` scaled by it
    double zoom_level = 0;
    // editor.tabSize
    int tab_size = 4;
    // editor.insertSpaces
    bool insert_spaces = true;
    // files.eol, with "auto" resolved: the line ending of new documents
    LineEnding line_ending = LineEnding::lf;
    // workbench.colorTheme: the `id` of a color theme, or its label when it has no id
    std::string color_theme;
    // workbench.editor.focusRecentEditorAfterClose: closing the active tab activates the tab used
    // before it, and not the neighbor
    bool focus_recent_editor_after_close = true;

    // editor.multiCursorModifier
    MultiCursorModifier multi_cursor_modifier = MultiCursorModifier::alt;
    // editor.columnSelection: dragging the mouse selects a rectangle
    bool column_selection = false;

    // files.autoSave
    AutoSave auto_save = AutoSave::off;
    // files.autoSaveDelay, in milliseconds: how long after its last change a document is saved
    // when `auto_save` is `after_delay`
    int auto_save_delay = 1000;

    // editor.wordWrap
    WordWrap word_wrap = WordWrap::off;
    // editor.wordWrapColumn
    int word_wrap_column = 80;

    // editor.mouseWheelScrollSensitivity, editor.fastScrollSensitivity and
    // editor.scrollPredominantAxis
    WheelSettings wheel;
    // editor.scrollBeyondLastLine: the editor scrolls until the last line is at the top
    bool scroll_beyond_last_line = true;
    // editor.scrollBeyondLastColumn: the characters that the editor scrolls past the longest line
    int scroll_beyond_last_column = 4;
    // editor.smoothScrolling: the editor moves to a new scroll position instead of jumping there
    bool smooth_scrolling = false;

    // editor.renderWhitespace
    RenderWhitespace render_whitespace = RenderWhitespace::selection;
    // editor.renderControlCharacters: control characters are drawn as symbols
    bool render_control_characters = true;

    // window.titleBarStyle: read when the window is created, so a change applies after a restart
    TitleBarStyle title_bar_style = TitleBarStyle::custom;

    // window.commandCenter: the title bar shows the box that opens the command palette
    bool command_center = true;
    // workbench.layoutControl.enabled: the title bar shows the buttons that toggle the side bar and
    // the panel
    bool layout_controls = true;

    // workbench.editor.restoreEditors: a workspace gets back the editors it was left with
    bool restore_editors = true;
    // workbench.editor.untitled.labelFormat
    UntitledLabel untitled_label = UntitledLabel::content;
    // window.restoreWindows
    RestoreWindows restore_windows = RestoreWindows::all;

    // workbench.statusBar.visible
    bool status_bar_visible = true;
    // workbench.sideBar.location
    SideBarLocation side_bar_location = SideBarLocation::left;
    // workbench.activityBar.location, where "default" is `standard`
    ActivityBarLocation activity_bar_location = ActivityBarLocation::standard;

    // workbench.editor.showTabs
    TabsMode show_tabs = TabsMode::multiple;
    // zenMode.showTabs: what `show_tabs` is while Zen Mode is on
    TabsMode zen_show_tabs = TabsMode::multiple;
    // editor.comments.insertSpace: a space follows a line comment token and goes inside a block
    // comment
    bool comments_insert_space = true;
    // editor.comments.ignoreEmptyLines: Toggle Line Comment leaves lines with nothing on them alone
    bool comments_ignore_empty_lines = true;
    // zenMode.fullScreen: turning on Zen Mode also puts the window into full screen
    bool zen_full_screen = true;
    // zenMode.centerLayout: turning on Zen Mode also centers the editor area
    bool zen_center_layout = true;
    // zenMode.hideStatusBar, zenMode.hideActivityBar: Zen Mode hides the part
    bool zen_hide_status_bar = true;
    bool zen_hide_activity_bar = true;
    // explorer.confirmDelete: the Explorer asks before it deletes
    bool explorer_confirm_delete = true;
    // `explorer.confirmDragAndDrop`: whether the Explorer asks before a drag moves items.
    bool explorer_confirm_drag_and_drop = true;
    // files.enableTrash: the Explorer moves what it deletes to the trash; without it a delete is
    // for good
    bool enable_trash = true;

    friend bool operator==(const EditorSettings&, const EditorSettings&) = default;

    // Absent keys take their defaults, and numbers outside their range are clamped (font size to
    // 6-100, zoom level to -8-8, tab size to 1-1024: the upper bound keeps a typo from making Tab
    // insert gigabytes; the word wrap column to 1-2^30; the columns scrolled beyond the last one to
    // 0-2^30; the auto save delay to 0 ms-24 h). A wheel sensitivity of 0 stands for 1, and a fast
    // scroll sensitivity of 0 or less for 5. A value
    // of the wrong type, an `editor.wordWrap` that is not "off", "on", "wordWrapColumn" or
    // "bounded", a `files.eol`
    // that is not one of "\n", "\r\n" and "auto", an `editor.multiCursorModifier` that is not "alt"
    // or "ctrlCmd", an `editor.renderWhitespace` that is not "none", "boundary", "selection",
    // "trailing" or "all", a `files.autoSave` that is not "off", "afterDelay", "onFocusChange" or
    // "onWindowChange", a `workbench.sideBar.location` that is not "left" or "right", a
    // `workbench.activityBar.location` that is not "default", "top", "bottom" or "hidden", a
    // `workbench.editor.showTabs` or `zenMode.showTabs` that is not "multiple", "single" or "none",
    // or a `window.titleBarStyle` that is not "custom" or "native", fails with
    // ErrorCode::invalid_argument and a message that names every offending key.
    static std::expected<EditorSettings, Error> from_settings(const Settings& settings,
                                                              const EditorDefaults& defaults);
};

// The user's `files.associations`: an object from a glob to a language id. An entry whose value is
// not a non-empty string, and a setting that is not an object, is skipped with a problem that
// names it, so that one mistake does not take the other associations away. The entries are in the
// order of the keys of the parsed object, which is alphabetical.
struct FileAssociations {
    std::vector<FileAssociation> associations;
    std::vector<std::string> problems;
};
FileAssociations read_file_associations(const Settings& settings);

} // namespace anvil::core
