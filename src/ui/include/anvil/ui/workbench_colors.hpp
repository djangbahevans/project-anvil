#pragma once

#include "anvil/core/token_theme.hpp"

#include <QColor>
#include <QObject>
#include <QPalette>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anvil::ui {

// A color of the workbench that a color theme can set: VS Code's key for it, where it is used, and
// what it is when the theme does not set it.
struct WorkbenchColorKey {
    std::string_view key;
    std::string_view description;
    // When the theme does not set the key, the value it gives this other key, if any. VS Code's
    // registry has such defaults, for example `sash.hoverBorder` is `focusBorder`.
    std::string_view fallback_key;
    // When that is not set either, the color of this role of the palette; with NoRole, `fixed`.
    QPalette::ColorRole role;
    QRgb fixed;
    // The default theme does not stand in for a theme that leaves the key out. VS Code gives such a
    // key no default, so that a part draws what it colors only when a theme sets it, or a default
    // of its own, which the theme store supplies.
    bool optional = false;
};

// The colors of the window's parts, by VS Code's workbench color key (`statusBar.background`,
// `tab.activeBackground`, ...). No part holds a color of its own: each asks for its keys here, so
// loading a color theme changes the table and every part repaints, with no change to a part.
//
// A key the theme does not set gets a color derived from the palette, which is how the window
// looks with no theme. The keys that exist are the table returned by `known_keys()`; asking for any
// other key is a programming error and answers magenta.
class WorkbenchColors : public QObject {
    Q_OBJECT

public:
    // The values a theme sets, by key.
    using Colors = std::unordered_map<std::string, QColor>;

    explicit WorkbenchColors(const QPalette& palette, QObject* parent = nullptr);

    // Every key, ordered by key.
    static std::span<const WorkbenchColorKey> known_keys();

    // Never invalid.
    QColor color(std::string_view key) const;
    // The theme sets this key itself, not through its fallback.
    bool is_themed(std::string_view key) const;

    // Replaces all the values of the theme. Keys that are not in the table are ignored. `changed`
    // is emitted once, and not at all when no value differs.
    void set_colors(const Colors& theme);
    // Derives the fallbacks from another palette; `changed` is emitted when it differs.
    void set_palette(const QPalette& palette);

    // How the theme styles the scopes that a highlighter gives (its `tokenColors`); null when
    // there is none. `changed` is emitted when the theme is another one.
    std::shared_ptr<const core::TokenTheme> token_theme() const {
        return token_theme_;
    }
    void set_token_theme(std::shared_ptr<const core::TokenTheme> theme);
    // Replaces the colors and the token theme together: `changed` is emitted once, and not at all
    // when neither differs.
    void set_theme(const Colors& theme, std::shared_ptr<const core::TokenTheme> token_theme);

signals:
    void changed();

private:
    static std::vector<std::optional<QColor>> themed_values(const Colors& theme);
    QColor resolve(std::size_t index, int depth) const;

    QPalette palette_;
    // Parallel to the table.
    std::vector<std::optional<QColor>> themed_;
    std::shared_ptr<const core::TokenTheme> token_theme_;
};

} // namespace anvil::ui
