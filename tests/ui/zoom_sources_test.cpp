#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Every size of the window follows the zoom, so a size in pixels that a part holds is read through
// `ui::Zoom`, or through a function of the part that does. These tests read the sources of
// `src/ui` and fail for a size that nothing says is scaled, so that a new part cannot start with
// a size that stays at zoom level 0.
namespace {

namespace fs = std::filesystem;

struct Source {
    std::string name;
    std::string text;
};

std::vector<Source> sources() {
    std::vector<Source> result;
    const fs::path root = ANVIL_UI_SOURCE_DIR;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const std::string extension = entry.path().extension().string();
        if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".hpp")) {
            continue;
        }
        std::ifstream file(entry.path());
        std::stringstream text;
        text << file.rdbuf();
        result.push_back({fs::relative(entry.path(), root).generic_string(), text.str()});
    }
    std::ranges::sort(result, {}, &Source::name);
    return result;
}

// The `constexpr` numbers that are the size of a part at zoom level 0, which the part reads
// through its zoom. A new one is added here when it is read through the zoom, and not before.
const std::set<std::string> level_zero_sizes = {
    "activity_bar.cpp:indicator_width",
    "editor_view.cpp:gutter_padding_px",
    "editor_view.cpp:text_padding_px",
    "explorer_view.cpp:button_horizontal_padding",
    "explorer_view.cpp:button_vertical_padding",
    "explorer_view.cpp:empty_spacing",
    "explorer_view.cpp:empty_top_padding",
    "explorer_view.cpp:header_font_pixels",
    "explorer_view.cpp:header_padding",
    "explorer_view.cpp:header_right_padding",
    "explorer_view.cpp:indent",
    "explorer_view.cpp:label_padding",
    "explorer_view.cpp:label_right_padding",
    "explorer_view.cpp:row_height",
    "explorer_view.cpp:twistie_size",
    "include/anvil/ui/activity_bar.hpp:button_size",
    "include/anvil/ui/activity_bar.hpp:icon_size",
    "include/anvil/ui/menu_style.hpp:check_column",
    "include/anvil/ui/menu_style.hpp:chevron_center_from_right",
    "include/anvil/ui/menu_style.hpp:glyph_size",
    "include/anvil/ui/menu_style.hpp:item_height",
    "include/anvil/ui/menu_style.hpp:label_left",
    "include/anvil/ui/menu_style.hpp:label_to_shortcut",
    "include/anvil/ui/menu_style.hpp:separator_height",
    "include/anvil/ui/menu_style.hpp:shortcut_right",
    "include/anvil/ui/menu_style.hpp:vertical_margin",
    "include/anvil/ui/panel.hpp:header_height",
    "include/anvil/ui/quick_pick.hpp:row_height",
    "include/anvil/ui/splitter.hpp:sash_width",
    "include/anvil/ui/status_bar.hpp:font_pixels",
    "include/anvil/ui/status_bar.hpp:height_pixels",
    "include/anvil/ui/status_bar.hpp:item_margin",
    "include/anvil/ui/status_bar.hpp:item_padding",
    "include/anvil/ui/tab_strip.hpp:action_width",
    "include/anvil/ui/tab_strip.hpp:close_button_size",
    "include/anvil/ui/tab_strip.hpp:font_pixels",
    "include/anvil/ui/tab_strip.hpp:height_pixels",
    "include/anvil/ui/tab_strip.hpp:minimum_tab_width",
    "include/anvil/ui/tab_strip.hpp:title_padding",
    "include/anvil/ui/title_bar.hpp:bar_height",
    "include/anvil/ui/title_bar.hpp:button_width",
    "include/anvil/ui/workbench.hpp:activity_bar_width",
    "include/anvil/ui/workbench.hpp:default_size",
    "include/anvil/ui/workbench.hpp:editor_minimum_height",
    "include/anvil/ui/workbench.hpp:editor_minimum_width",
    "include/anvil/ui/workbench.hpp:panel_minimum_height",
    "include/anvil/ui/workbench.hpp:panel_minimum_width",
    "include/anvil/ui/workbench.hpp:sidebar_minimum_width",
    "panel.cpp:action_size",
    "panel.cpp:header_right_padding",
    "panel.cpp:tab_padding",
    "panel.cpp:title_font_pixels",
    "quick_pick.cpp:corner_radius_px",
    "quick_pick.cpp:header_bottom_padding_px",
    "quick_pick.cpp:header_padding_px",
    "quick_pick.cpp:highlight_radius_px",
    "quick_pick.cpp:input_horizontal_padding_px",
    "quick_pick.cpp:input_radius_px",
    "quick_pick.cpp:input_vertical_padding_px",
    "quick_pick.cpp:list_slack_px",
    "quick_pick.cpp:max_width_px",
    "quick_pick.cpp:row_bottom_text_gap_px",
    "quick_pick.cpp:row_inset_px",
    "quick_pick.cpp:shadow_blur_px",
    "quick_pick.cpp:text_padding_px",
    "quick_pick.cpp:window_margin_px",
    "sidebar.cpp:title_font_pixels",
    "sidebar.cpp:title_height_pixels",
    "sidebar.cpp:title_padding",
    "sidebar.cpp:title_right_padding",
    "tab_strip.cpp:close_cross_half_size",
    "tab_strip.cpp:dot_diameter",
    "tab_strip.cpp:hover_radius",
    "title_bar.cpp:command_center_glyph",
    "title_bar.cpp:command_center_height",
    "title_bar.cpp:command_center_margin",
    "title_bar.cpp:command_center_max_width",
    "title_bar.cpp:command_center_min_width",
    "title_bar.cpp:command_center_padding",
    "title_bar.cpp:icon_margin",
    "title_bar.cpp:icon_size",
    "title_bar.cpp:layout_button_size",
    "title_bar.cpp:layout_spacer_width",
    "title_bar.cpp:menu_item_padding",
    "title_bar.cpp:radius",
    "title_bar.cpp:text_pixels",
};

// The `constexpr` numbers that are not a size in pixels, and why.
const std::map<std::string, std::string> not_sizes = {
    {"editor_area.cpp:scale", "a ratio of the width of the area, not a size in pixels"},
    {"explorer_model.cpp:poll_interval_ms", "a time"},
    {"explorer_view.cpp:hover_expand_ms", "a time"},
    {"include/anvil/ui/editor_area.hpp:centered_margin_ratio", "a ratio of the width of the area"},
    {"include/anvil/ui/editor_view.hpp:max_line_characters", "a count of characters"},
    {"include/anvil/ui/explorer_model.hpp:batch_size", "a count of entries"},
    {"include/anvil/ui/menu_style.hpp:frame_width", "the 1 px frame of a menu, a hairline"},
    {"include/anvil/ui/quick_pick.hpp:max_visible_rows", "a count of rows"},
    {"include/anvil/ui/window_frame.hpp:handle_width",
     "the grab area at the edge of a window that has no frame of its own, which the window "
     "system's own frame has at one size whatever the zoom"},
    {"main_window.cpp:language_priority", "a priority of a status bar item"},
    {"main_window.cpp:position_priority", "a priority of a status bar item"},
    {"panel.cpp:action_pen_width", "the width of a stroke, in units of the level 0 size, "
                                   "multiplied by the factor where it is drawn"},
    {"quick_pick.cpp:detail_row_lines", "a count of lines"},
    {"tab_strip.cpp:cross_pen_width", "the width of a stroke, in units of the level 0 size, "
                                      "multiplied by the factor where it is drawn"},
    {"workbench_colors.cpp:max_chain", "a count of links"},
};

TEST(ZoomSourcesTest, EveryConstexprNumberIsASizeThatIsReadThroughTheZoomOrSaysWhyItIsNot) {
    const std::regex declaration(
        R"(^\s*(?:static\s+)?constexpr\s+(?:int|qreal|double)\s+(\w+)\s*=)", std::regex::multiline);
    std::set<std::string> found;
    for (const Source& source : sources()) {
        for (std::sregex_iterator it(source.text.begin(), source.text.end(), declaration), end;
             it != end; ++it) {
            const std::string name = source.name + ":" + (*it)[1].str();
            found.insert(name);
            EXPECT_TRUE(level_zero_sizes.contains(name) || not_sizes.contains(name))
                << name
                << " is a number in pixels that no test knows: read it through ui::Zoom and add "
                   "it to level_zero_sizes, or add it to not_sizes with the reason";
        }
    }
    for (const std::string& name : level_zero_sizes) {
        EXPECT_TRUE(found.contains(name)) << name << " is listed and no longer declared";
    }
    for (const auto& [name, reason] : not_sizes) {
        EXPECT_TRUE(found.contains(name)) << name << " is listed and no longer declared";
        EXPECT_FALSE(reason.empty()) << name;
    }
}

// The calls that give a widget or a font a size in pixels.
const std::regex sizing_call(
    R"(\b(setFixedHeight|setFixedWidth|setFixedSize|setIconSize|setPixelSize|setMinimumWidth|setMinimumHeight|setMinimumSize|setHandleWidth)\s*\()");

// What in the statement of such a call says that its size is scaled.
const std::vector<std::string> scaled_words = {
    "zoomed", "capped", "scaled", "strip_height()", "bar_height()", "row_size()", "height_now()"};

// A call whose size is scaled before it gets there, with the reason.
struct Exempt {
    std::string file;
    std::string statement;
    std::string reason;
};
const std::vector<Exempt> exempt_calls = {
    {"codicons.cpp", "setPixelSize(size)", "the size is the one the caller asks the icon for"},
    {"editor_view.cpp", "setPixelSize(pixel_size)",
     "the caller passes the font size scaled by the level of the settings"},
    {"find_bar.cpp", "fontMetrics", "the width of a text in the font of the bar, which is scaled"},
    {"menu_style.cpp", "setPixelSize(size)", "the callers pass the glyph size scaled"},
    {"quick_pick.cpp", "setFixedHeight(list_height)", "made of rows that are scaled"},
    {"workbench.cpp", "setMinimumWidth(sidebar_minimum())", "sidebar_minimum() scales it"},
    {"workbench.cpp", "panel_minimum()", "panel_minimum() scales it"},
    {"quick_pick.cpp", "setFixedWidth(std::max(0, width))", "made of a width that is scaled"},
    {"title_bar.cpp", "setPixelSize(size)", "the callers pass the glyph size scaled"},
    {"explorer_view.cpp", "setFixedWidth(width)",
     "the width of the viewport, from where the field is, or the width of the field"},
    {"explorer_view.cpp", "setFixedHeight(problem_->heightForWidth(width))",
     "the height of the text of the label in its font, which is scaled"},
};

TEST(ZoomSourcesTest, EveryCallThatGivesASizeInPixelsReadsItThroughTheZoomOrSaysWhyNot) {
    for (const Source& source : sources()) {
        for (std::sregex_iterator it(source.text.begin(), source.text.end(), sizing_call), end;
             it != end; ++it) {
            const auto begin = static_cast<std::size_t>(it->position());
            const std::size_t semicolon = source.text.find(';', begin);
            const std::string statement = source.text.substr(begin, semicolon - begin);
            const bool scaled = std::ranges::any_of(scaled_words, [&](const std::string& word) {
                return statement.find(word) != std::string::npos;
            });
            const bool exempt = std::ranges::any_of(exempt_calls, [&](const Exempt& entry) {
                return source.name == entry.file &&
                       statement.find(entry.statement) != std::string::npos &&
                       !entry.reason.empty();
            });
            const auto line =
                1 + std::count(source.text.begin(),
                               source.text.begin() + static_cast<std::ptrdiff_t>(begin), '\n');
            EXPECT_TRUE(scaled || exempt)
                << source.name << ":" << line << ": " << statement
                << " gives a size in pixels that is not read through ui::Zoom; scale it, or list "
                   "the call in exempt_calls with the reason";
        }
    }
}

} // namespace
