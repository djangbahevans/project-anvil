#include "anvil/ui/workbench_colors.hpp"

#include <algorithm>

namespace anvil::ui {
namespace {

constexpr WorkbenchColorKey from_palette(std::string_view key, std::string_view description,
                                         QPalette::ColorRole role) {
    return {key, description, {}, role, 0};
}

constexpr WorkbenchColorKey chained(std::string_view key, std::string_view description,
                                    std::string_view fallback_key, QPalette::ColorRole role) {
    return {key, description, fallback_key, role, 0};
}

constexpr WorkbenchColorKey optional_color(std::string_view key, std::string_view description,
                                           QPalette::ColorRole role) {
    return {key, description, {}, role, 0, true};
}

constexpr WorkbenchColorKey fixed_color(std::string_view key, std::string_view description,
                                        QRgb color) {
    return {key, description, {}, QPalette::NoRole, color};
}

// Ordered by key, so that a key is found by bisection.
constexpr WorkbenchColorKey table[] = {
    from_palette("activityBar.activeBorder",
                 "Marker on the edge of the activity bar button of the open view container.",
                 QPalette::Highlight),
    from_palette("activityBar.background", "Background of the activity bar.", QPalette::Window),
    from_palette("activityBar.foreground", "Icons of the active button of the activity bar.",
                 QPalette::WindowText),
    from_palette("activityBar.inactiveForeground",
                 "Icons of the inactive buttons of the activity bar.", QPalette::PlaceholderText),
    from_palette("activityBarBadge.background",
                 "Background of the count badge on an activity bar button.", QPalette::Highlight),
    from_palette("activityBarBadge.foreground",
                 "Text of the count badge on an activity bar button.", QPalette::HighlightedText),
    from_palette("button.background", "Background of a button.", QPalette::Highlight),
    from_palette("button.foreground", "Text of a button.", QPalette::HighlightedText),
    chained("button.hoverBackground", "Background of a button under the mouse.",
            "button.background", QPalette::Highlight),
    chained("commandCenter.activeBackground",
            "Background of the command center while the pointer is over it.",
            "toolbar.hoverBackground", QPalette::Midlight),
    chained("commandCenter.activeBorder",
            "Border of the command center while the pointer is over it.", "commandCenter.border",
            QPalette::Mid),
    chained("commandCenter.activeForeground",
            "Text of the command center while the pointer is over it.", "commandCenter.foreground",
            QPalette::WindowText),
    optional_color("commandCenter.background", "Background of the command center.",
                   QPalette::Window),
    from_palette("commandCenter.border", "Border of the command center.", QPalette::Mid),
    chained("commandCenter.foreground", "Text of the command center.", "titleBar.activeForeground",
            QPalette::WindowText),
    chained("commandCenter.inactiveBorder",
            "Border of the command center in a window that is not active.", "commandCenter.border",
            QPalette::Mid),
    chained("commandCenter.inactiveForeground",
            "Text of the command center in a window that is not active.",
            "titleBar.inactiveForeground", QPalette::PlaceholderText),
    optional_color("contrastBorder",
                   "Border that high contrast themes put around elements to set them apart.",
                   QPalette::WindowText),
    from_palette("descriptionForeground",
                 "Secondary text: descriptions next to a label, and hints.",
                 QPalette::PlaceholderText),
    from_palette("editor.background", "Background of the text editor.", QPalette::Base),
    fixed_color("editor.findMatchBackground", "Background of the find match that is selected.",
                qRgba(255, 190, 0, 90)),
    fixed_color("editor.findMatchHighlightBackground", "Background of the other find matches.",
                qRgba(255, 190, 0, 90)),
    from_palette("editor.foreground", "Default text color of the text editor.", QPalette::Text),
    from_palette("editor.lineHighlightBackground", "Background of the line the caret is on.",
                 QPalette::AlternateBase),
    from_palette("editor.selectionBackground", "Background of selected text in the editor.",
                 QPalette::Highlight),
    from_palette("editorCursor.foreground", "The caret of the editor.", QPalette::Text),
    optional_color("editorGroup.border", "Border between the groups of the editor area.",
                   QPalette::Mid),
    optional_color("editorGroup.dropBackground",
                   "Shade over the part of a group where a dragged tab would go.",
                   QPalette::Highlight),
    from_palette("editorGroupHeader.tabsBackground",
                 "Background of the strip of tabs above an editor.", QPalette::Window),
    optional_color("editorGroupHeader.tabsBorder", "Border below the strip of tabs.",
                   QPalette::Mid),
    chained("editorGutter.background", "Background of the gutter with the line numbers.",
            "editor.background", QPalette::Window),
    from_palette("editorLineNumber.activeForeground", "Line number of the line the caret is on.",
                 QPalette::Text),
    from_palette("editorLineNumber.foreground", "Line numbers in the gutter.",
                 QPalette::PlaceholderText),
    from_palette("editorWhitespace.foreground",
                 "Marks drawn for spaces and tabs by editor.renderWhitespace.",
                 QPalette::PlaceholderText),
    from_palette("focusBorder",
                 "Border of the focused control; the default of other focus-related borders.",
                 QPalette::Highlight),
    from_palette("input.background", "Background of an input field.", QPalette::Base),
    chained("input.border", "Border of an input field.", "widget.border", QPalette::WindowText),
    from_palette("input.foreground", "Text of an input field.", QPalette::Text),
    from_palette("input.placeholderForeground", "Placeholder text of an input field.",
                 QPalette::PlaceholderText),
    fixed_color("inputValidation.errorBackground",
                "Background of the message under an input field that holds a value that is not "
                "accepted.",
                qRgb(0x5a, 0x1d, 0x1d)),
    fixed_color("inputValidation.errorBorder",
                "Border of the message under an input field that holds a value that is not "
                "accepted.",
                qRgb(0xbe, 0x11, 0x00)),
    fixed_color("inputValidation.errorForeground",
                "Text of the message under an input field that holds a value that is not "
                "accepted.",
                qRgb(0xff, 0xff, 0xff)),
    from_palette("list.activeSelectionBackground",
                 "Background of the selected row of a list or tree that has the keyboard focus.",
                 QPalette::Highlight),
    from_palette("list.activeSelectionForeground",
                 "Text of the selected row of a list or tree that has the keyboard focus.",
                 QPalette::HighlightedText),
    from_palette("list.dropBackground",
                 "Background of the rows of the folder that a drag would drop into.",
                 QPalette::Midlight),
    from_palette("list.highlightForeground",
                 "The characters of a list item that match what was typed.", QPalette::Link),
    from_palette("list.hoverBackground", "Background of the row of a list or tree under the mouse.",
                 QPalette::Midlight),
    chained("list.hoverForeground", "Text of the row of a list or tree under the mouse.",
            "sideBar.foreground", QPalette::WindowText),
    from_palette("list.inactiveSelectionBackground",
                 "Background of the selected row of a list or tree that does not have the "
                 "keyboard focus.",
                 QPalette::Mid),
    chained("list.inactiveSelectionForeground",
            "Text of the selected row of a list or tree that does not have the keyboard focus.",
            "sideBar.foreground", QPalette::WindowText),
    from_palette("menu.background", "Background of the menus of the menu bar.", QPalette::Window),
    chained("menu.border", "Border around a menu.", "widget.border", QPalette::WindowText),
    from_palette("menu.foreground", "Text of the items of a menu.", QPalette::WindowText),
    from_palette("menu.selectionBackground", "Background of the item of a menu that is chosen.",
                 QPalette::Highlight),
    from_palette("menu.selectionForeground", "Text of the item of a menu that is chosen.",
                 QPalette::HighlightedText),
    from_palette("menu.separatorBackground", "Line between groups of items of a menu.",
                 QPalette::Mid),
    from_palette("panel.background", "Background of the bottom panel.", QPalette::Base),
    from_palette("panel.border", "Border between the panel and the editor area.", QPalette::Mid),
    from_palette("panelTitle.activeBorder", "Line under the active title in the panel header.",
                 QPalette::Highlight),
    from_palette("panelTitle.activeForeground", "The active title in the panel header.",
                 QPalette::Text),
    from_palette("panelTitle.inactiveForeground", "The inactive titles in the panel header.",
                 QPalette::PlaceholderText),
    from_palette("pickerGroup.border", "Line above a group of items of the quick pick.",
                 QPalette::Mid),
    from_palette("quickInput.background", "Background of the quick pick and the command palette.",
                 QPalette::Base),
    from_palette("quickInput.foreground", "Text of the quick pick and the command palette.",
                 QPalette::Text),
    from_palette("quickInputList.focusBackground",
                 "Background of the chosen item of the quick pick list.", QPalette::Highlight),
    from_palette("quickInputList.focusForeground",
                 "Text of the chosen item of the quick pick list.", QPalette::HighlightedText),
    chained("sash.hoverBorder", "Border of a splitter handle that is hovered or dragged.",
            "focusBorder", QPalette::Highlight),
    optional_color("scrollbar.background", "Track of the scroll bars.", QPalette::Window),
    optional_color("scrollbarSlider.activeBackground",
                   "Slider of a scroll bar while it is dragged.", QPalette::Dark),
    optional_color("scrollbarSlider.background", "Slider of a scroll bar.", QPalette::Mid),
    optional_color("scrollbarSlider.hoverBackground", "Slider of a scroll bar under the pointer.",
                   QPalette::Midlight),
    from_palette("sideBar.background", "Background of the sidebar.", QPalette::Window),
    from_palette("sideBar.border", "Border between the sidebar and the editor area.",
                 QPalette::Mid),
    from_palette("sideBar.foreground", "Text of the sidebar.", QPalette::WindowText),
    from_palette("sideBarTitle.foreground", "Title at the top of the sidebar.",
                 QPalette::WindowText),
    from_palette("statusBar.background", "Background of the status bar.", QPalette::Window),
    optional_color("statusBar.border", "Border above the status bar.", QPalette::Mid),
    from_palette("statusBar.foreground", "Text of the status bar.", QPalette::WindowText),
    from_palette("statusBarItem.activeBackground",
                 "Background of a status bar item while the mouse button is down.", QPalette::Mid),
    from_palette("statusBarItem.hoverBackground",
                 "Background of a status bar item under the mouse.", QPalette::Midlight),
    from_palette("tab.activeBackground", "Background of the selected tab.", QPalette::Base),
    optional_color("tab.activeBorder", "Line below the selected tab.", QPalette::Highlight),
    from_palette("tab.activeBorderTop", "Line on top of the selected tab.", QPalette::Highlight),
    from_palette("tab.activeForeground", "Title of the selected tab.", QPalette::Text),
    from_palette("tab.border", "Border between tabs.", QPalette::Mid),
    chained("tab.dragAndDropBorder", "Line where a dragged tab would go among the tabs.",
            "tab.activeForeground", QPalette::Text),
    optional_color("tab.hoverBackground", "Background of a tab under the mouse.",
                   QPalette::Midlight),
    from_palette("tab.inactiveBackground", "Background of the tabs that are not selected.",
                 QPalette::Window),
    from_palette("tab.inactiveForeground", "Titles of the tabs that are not selected.",
                 QPalette::WindowText),
    chained("tab.unfocusedActiveBackground",
            "Background of the selected tab of a group that does not have the focus.",
            "tab.activeBackground", QPalette::Base),
    optional_color("tab.unfocusedActiveBorderTop",
                   "Line on top of the selected tab of a group that does not have the focus.",
                   QPalette::Highlight),
    chained("tab.unfocusedActiveForeground",
            "Title of the selected tab of a group that does not have the focus.",
            "tab.activeForeground", QPalette::Text),
    chained("tab.unfocusedInactiveBackground",
            "Background of the other tabs of a group that does not have the focus.",
            "tab.inactiveBackground", QPalette::Window),
    chained("tab.unfocusedInactiveForeground",
            "Titles of the other tabs of a group that does not have the focus.",
            "tab.inactiveForeground", QPalette::WindowText),
    from_palette("titleBar.activeBackground",
                 "Background of the title bar of the window that is active.", QPalette::Window),
    from_palette("titleBar.activeForeground",
                 "Text and icons of the title bar of the active window.", QPalette::WindowText),
    from_palette("titleBar.border", "Border below the title bar, and around the window.",
                 QPalette::Mid),
    chained("titleBar.inactiveBackground",
            "Background of the title bar of a window that is not active.",
            "titleBar.activeBackground", QPalette::Window),
    chained("titleBar.inactiveForeground",
            "Text and icons of the title bar of a window that is not active.",
            "titleBar.activeForeground", QPalette::PlaceholderText),
    optional_color(
        "toolbar.hoverBackground",
        "Background of a small button, such as the close button of a tab, under the mouse.",
        QPalette::Midlight),
    from_palette("widget.border", "Border of popups such as the quick pick.", QPalette::WindowText),
    // No shadow where the theme and the registry give none, as in the high contrast themes.
    {"widget.shadow",
     "Shadow around popups such as the quick pick.",
     {},
     QPalette::NoRole,
     0,
     true},
};

constexpr int max_chain = 4;
const QColor unknown_key_color(255, 0, 255);

std::optional<std::size_t> find(std::string_view key) {
    const auto found = std::ranges::lower_bound(table, key, {}, &WorkbenchColorKey::key);
    if (found == std::end(table) || found->key != key) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - std::begin(table));
}

} // namespace

WorkbenchColors::WorkbenchColors(const QPalette& palette, QObject* parent)
    : QObject(parent), palette_(palette), themed_(std::size(table)) {}

std::span<const WorkbenchColorKey> WorkbenchColors::known_keys() {
    return table;
}

QColor WorkbenchColors::color(std::string_view key) const {
    const auto index = find(key);
    return index ? resolve(*index, 0) : unknown_key_color;
}

bool WorkbenchColors::is_themed(std::string_view key) const {
    const auto index = find(key);
    return index && themed_[*index].has_value();
}

QColor WorkbenchColors::resolve(std::size_t index, int depth) const {
    if (themed_[index]) {
        return *themed_[index];
    }
    const WorkbenchColorKey& entry = table[index];
    if (!entry.fallback_key.empty() && depth < max_chain) {
        if (const auto other = find(entry.fallback_key)) {
            return resolve(*other, depth + 1);
        }
    }
    return entry.role == QPalette::NoRole ? QColor::fromRgba(entry.fixed)
                                          : palette_.color(entry.role);
}

std::vector<std::optional<QColor>> WorkbenchColors::themed_values(const Colors& theme) {
    std::vector<std::optional<QColor>> values(std::size(table));
    for (std::size_t i = 0; i < std::size(table); ++i) {
        if (const auto value = theme.find(std::string(table[i].key));
            value != theme.end() && value->second.isValid()) {
            values[i] = value->second;
        }
    }
    return values;
}

void WorkbenchColors::set_colors(const Colors& theme) {
    auto values = themed_values(theme);
    if (values == themed_) {
        return;
    }
    themed_ = std::move(values);
    emit changed();
}

void WorkbenchColors::set_theme(const Colors& theme,
                                std::shared_ptr<const core::TokenTheme> token_theme) {
    auto values = themed_values(theme);
    if (values == themed_ && token_theme == token_theme_) {
        return;
    }
    themed_ = std::move(values);
    token_theme_ = std::move(token_theme);
    emit changed();
}

void WorkbenchColors::set_palette(const QPalette& palette) {
    if (palette == palette_) {
        return;
    }
    palette_ = palette;
    emit changed();
}

void WorkbenchColors::set_token_theme(std::shared_ptr<const core::TokenTheme> theme) {
    if (theme == token_theme_) {
        return;
    }
    token_theme_ = std::move(theme);
    emit changed();
}

} // namespace anvil::ui
