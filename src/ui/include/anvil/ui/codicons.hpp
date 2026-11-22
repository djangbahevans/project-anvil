#pragma once

#include <QColor>
#include <QIcon>
#include <QString>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::ui {

// VS Code's Codicons: the icon font that its activity bar, status bar, tree views and palette draw
// from. The font ships inside the editor (see THIRD_PARTY_NOTICES.md for its license) and loads
// the first time an icon is drawn.

struct CodiconEntry {
    std::string_view name;
    char16_t code;
};

// Every icon of the font, ordered by name.
std::span<const CodiconEntry> codicon_table();

// The character that draws the icon in the font, or nothing for a name the font does not have.
std::optional<char16_t> codicon(std::string_view name);

// The family of the loaded font, loading it on the first call; empty when it cannot be loaded.
QString codicon_font_family();

// The icon drawn from the font at `size` logical pixels in `color`, for the normal, active and
// selected states, and for the disabled state in the same color at 40 % opacity as VS Code does.
// A null icon for an unknown name.
QIcon codicon_icon(std::string_view name, const QColor& color, int size);

// A piece of a label that has `$(name)` markers: either text, or an icon.
struct LabelSegment {
    QString text = {};
    // Not empty for an icon segment, and then `text` is empty.
    std::string icon = {};
    // The part after `~` in `$(sync~spin)`.
    std::string modifier = {};

    bool is_icon() const {
        return !icon.empty();
    }
    bool operator==(const LabelSegment&) const = default;
};

// Splits a label such as `$(sync~spin) Syncing` into segments. A marker is `$(`, a name of ASCII
// letters, digits and hyphens, optionally `~` and letters, and `)`. Anything else that looks like a
// marker is text, and a backslash before a marker makes it text without the backslash. A name the
// font does not have still gives an icon segment, because the caller decides what to draw for it.
// Neighboring pieces of text are one segment.
std::vector<LabelSegment> split_label_with_icons(const QString& label);

} // namespace anvil::ui
