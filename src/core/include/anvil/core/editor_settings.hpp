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
};

// The editor's settings, read from `Settings` with VS Code's key names, ranges and defaults.
struct EditorSettings {
    static constexpr int min_font_size = 6;
    static constexpr int max_font_size = 100;
    static constexpr int max_tab_size = 1024;

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

    friend bool operator==(const EditorSettings&, const EditorSettings&) = default;

    // Absent keys take their defaults, and numbers outside their range are clamped (font size to
    // 6-100, tab size to 1-1024: the upper bound keeps a typo from making Tab insert gigabytes).
    // A value of the wrong type, or a `files.eol` that is not one of "\n", "\r\n" and "auto", fails
    // with ErrorCode::invalid_argument and a message that names every offending key.
    static std::expected<EditorSettings, Error> from_settings(const Settings& settings,
                                                              const EditorDefaults& defaults);
};

} // namespace anvil::core
