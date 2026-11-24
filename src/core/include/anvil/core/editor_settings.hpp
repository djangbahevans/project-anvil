#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/settings.hpp"
#include "anvil/core/text_file.hpp"

#include <expected>
#include <string>

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
};

// Whether the window draws its own title bar (`custom`) or has the system's frame (`native`).
enum class TitleBarStyle { custom, native };

// files.autoSave: when a document with a file and unsaved changes is saved without being asked.
enum class AutoSave { off, after_delay, on_focus_change, on_window_change };

// editor.renderWhitespace: which spaces and tabs the editor draws a mark for.
enum class RenderWhitespace { none, boundary, selection, trailing, all };

// editor.multiCursorModifier: the key that makes a click add a cursor. `ctrl_cmd` is Ctrl, or Cmd
// on macOS.
enum class MultiCursorModifier { alt, ctrl_cmd };

// The editor's settings, read from `Settings` with VS Code's key names, ranges and defaults.
struct EditorSettings {
    static constexpr int min_font_size = 6;
    static constexpr int max_font_size = 100;
    static constexpr int max_tab_size = 1024;
    static constexpr int max_auto_save_delay = 24 * 60 * 60 * 1000;

    // editor.fontFamily
    std::string font_family;
    // editor.fontSize, in pixels
    int font_size = 14;
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

    friend bool operator==(const EditorSettings&, const EditorSettings&) = default;

    // Absent keys take their defaults, and numbers outside their range are clamped (font size to
    // 6-100, tab size to 1-1024: the upper bound keeps a typo from making Tab insert gigabytes;
    // the auto save delay to 0 ms-24 h). A value of the wrong type, a `files.eol` that is not one
    // of "\n", "\r\n" and "auto", an `editor.multiCursorModifier` that is not "alt" or "ctrlCmd",
    // an `editor.renderWhitespace` that is not "none", "boundary", "selection", "trailing" or
    // "all", a `files.autoSave` that is not "off", "afterDelay", "onFocusChange" or
    // "onWindowChange", or a `window.titleBarStyle` that is not "custom" or "native", fails with
    // ErrorCode::invalid_argument and a message that names every offending key.
    static std::expected<EditorSettings, Error> from_settings(const Settings& settings,
                                                              const EditorDefaults& defaults);
};

} // namespace anvil::core
