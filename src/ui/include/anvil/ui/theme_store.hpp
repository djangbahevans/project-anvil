#pragma once

#include "anvil/core/color_theme.hpp"
#include "anvil/core/log.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QObject>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::ui {

// The color themes that ship with the editor and the one that is applied. Applying a theme gives
// its colors to the workbench colors in one step, so every part repaints once.
//
// A theme that leaves a key unset gets what VS Code's color registry would give it, as far as the
// editor has it: the value of the key the table says it defaults to (the gutter follows the editor
// background), and otherwise the default theme of the same kind, Dark 2026 or Light 2026.
//
// A theme is named by its `id`, or by its label when it has no id, as the `workbench.colorTheme`
// setting does. A name that is not a theme and a theme that cannot be loaded leave the applied
// theme as it is and are reported once, with the same message not repeated until a theme loads.
class ThemeStore : public QObject {
    Q_OBJECT

public:
    struct Entry {
        std::string id;
        std::string label;
        core::ThemeKind kind = core::ThemeKind::dark;
        std::filesystem::path file;
    };

    // `directory` holds `themes.json` and the theme files; a missing or unreadable list leaves
    // the store with no themes, which is reported. The colors and the logger must outlive the
    // store.
    ThemeStore(WorkbenchColors& colors, const std::filesystem::path& directory,
               std::shared_ptr<core::Logger> logger, QObject* parent = nullptr);

    const std::vector<Entry>& themes() const {
        return themes_;
    }

    // The id of the applied theme; empty before one has been.
    const std::string& current() const {
        return current_;
    }
    // The applied theme, for what its rules say about syntax colors.
    const core::ColorTheme* color_theme() const {
        return theme_ ? &*theme_ : nullptr;
    }

    // Applies the theme the name refers to. When no theme has been applied yet and `name` cannot
    // be, `fallback` is tried quietly, so that the editor starts with a theme. False when `name`
    // was not applied.
    bool select(std::string_view name, std::string_view fallback = {});

signals:
    // A theme was applied.
    void changed();
    // A name or a theme could not be used. `message` says which and why.
    void load_failed(const QString& message);

private:
    const Entry* find(std::string_view name) const;
    bool apply(const Entry& entry, std::string* problem);
    WorkbenchColors::Colors compose(const Entry& entry, const core::ColorTheme& theme);
    void report(const std::string& problem);

    WorkbenchColors& colors_;
    std::shared_ptr<core::Logger> logger_;
    std::vector<Entry> themes_;
    std::string current_;
    std::optional<core::ColorTheme> theme_;
    std::string last_problem_;
};

} // namespace anvil::ui
