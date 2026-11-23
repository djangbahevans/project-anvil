#include "anvil/core/editor_settings.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace anvil::core {
namespace {

bool is_blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

int clamped(double value, int minimum, int maximum) {
    return static_cast<int>(
        std::lround(std::clamp(value, static_cast<double>(minimum), static_cast<double>(maximum))));
}

} // namespace

std::expected<EditorSettings, Error> EditorSettings::from_settings(const Settings& settings,
                                                                   const EditorDefaults& defaults) {
    EditorSettings result;
    result.font_family = defaults.font_family;
    result.font_size = defaults.font_size;
    result.line_ending = defaults.auto_line_ending;
    result.color_theme = defaults.color_theme;
    std::vector<std::string> problems;

    if (settings.contains("editor.fontFamily")) {
        if (const auto family = settings.get<std::string>("editor.fontFamily")) {
            if (!is_blank(*family)) {
                result.font_family = *family;
            }
        } else {
            problems.emplace_back("editor.fontFamily must be a string");
        }
    }
    if (settings.contains("editor.fontSize")) {
        if (const auto size = settings.get<double>("editor.fontSize")) {
            result.font_size = clamped(*size, min_font_size, max_font_size);
        } else {
            problems.emplace_back("editor.fontSize must be a number");
        }
    }
    if (settings.contains("editor.tabSize")) {
        if (const auto size = settings.get<std::int64_t>("editor.tabSize")) {
            result.tab_size = clamped(static_cast<double>(*size), 1, max_tab_size);
        } else {
            problems.emplace_back("editor.tabSize must be an integer");
        }
    }
    if (settings.contains("editor.insertSpaces")) {
        if (const auto insert = settings.get<bool>("editor.insertSpaces")) {
            result.insert_spaces = *insert;
        } else {
            problems.emplace_back("editor.insertSpaces must be true or false");
        }
    }
    if (settings.contains("workbench.colorTheme")) {
        if (const auto theme = settings.get<std::string>("workbench.colorTheme")) {
            if (!is_blank(*theme)) {
                result.color_theme = *theme;
            }
        } else {
            problems.emplace_back("workbench.colorTheme must be a string");
        }
    }
    if (settings.contains("window.commandCenter")) {
        if (const auto shown = settings.get<bool>("window.commandCenter")) {
            result.command_center = *shown;
        } else {
            problems.emplace_back("window.commandCenter must be true or false");
        }
    }
    if (settings.contains("workbench.layoutControl.enabled")) {
        if (const auto shown = settings.get<bool>("workbench.layoutControl.enabled")) {
            result.layout_controls = *shown;
        } else {
            problems.emplace_back("workbench.layoutControl.enabled must be true or false");
        }
    }
    if (settings.contains("window.titleBarStyle")) {
        const auto style = settings.get<std::string>("window.titleBarStyle");
        if (style && *style == "custom") {
            result.title_bar_style = TitleBarStyle::custom;
        } else if (style && *style == "native") {
            result.title_bar_style = TitleBarStyle::native;
        } else {
            problems.emplace_back("window.titleBarStyle must be \"custom\" or \"native\"");
        }
    }
    if (settings.contains("workbench.editor.focusRecentEditorAfterClose")) {
        if (const auto recent =
                settings.get<bool>("workbench.editor.focusRecentEditorAfterClose")) {
            result.focus_recent_editor_after_close = *recent;
        } else {
            problems.emplace_back(
                "workbench.editor.focusRecentEditorAfterClose must be true or false");
        }
    }
    if (settings.contains("files.eol")) {
        const auto eol = settings.get<std::string>("files.eol");
        if (eol == "\n") {
            result.line_ending = LineEnding::lf;
        } else if (eol == "\r\n") {
            result.line_ending = LineEnding::crlf;
        } else if (eol == "auto") {
            result.line_ending = defaults.auto_line_ending;
        } else {
            problems.emplace_back(R"(files.eol must be "\n", "\r\n" or "auto")");
        }
    }

    if (problems.empty()) {
        return result;
    }
    std::string message;
    for (const std::string& problem : problems) {
        message += message.empty() ? problem : "; " + problem;
    }
    return std::unexpected(Error{ErrorCode::invalid_argument, std::move(message)});
}

} // namespace anvil::core
