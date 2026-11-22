#include "anvil/ui/theme_store.hpp"

#include "anvil/core/jsonc.hpp"
#include "anvil/platform/file.hpp"

#include <nlohmann/json.hpp>

namespace anvil::ui {

ThemeStore::ThemeStore(WorkbenchColors& colors, const std::filesystem::path& directory,
                       std::shared_ptr<core::Logger> logger, QObject* parent)
    : QObject(parent), colors_(colors), logger_(std::move(logger)) {
    const auto text = platform::read_file(directory / "themes.json");
    if (!text) {
        logger_->log(core::LogLevel::error, "theme",
                     "The list of color themes cannot be read: " + text.error().message);
        return;
    }
    const auto list = core::parse_jsonc(*text);
    if (!list || !list->is_array()) {
        logger_->log(core::LogLevel::error, "theme",
                     "The list of color themes is not valid: " +
                         (list ? std::string("an array is expected") : list.error().message));
        return;
    }
    for (const nlohmann::json& item : *list) {
        if (!item.is_object() || !item.contains("path") || !item["path"].is_string() ||
            !(item.contains("id") || item.contains("label"))) {
            continue;
        }
        Entry entry;
        entry.label = item.value("label", std::string());
        entry.id = item.value("id", entry.label);
        entry.kind = core::theme_kind_from_ui_theme(item.value("uiTheme", std::string()));
        entry.file = directory / item["path"].get<std::string>();
        themes_.push_back(std::move(entry));
    }
}

const ThemeStore::Entry* ThemeStore::find(std::string_view name) const {
    for (const Entry& entry : themes_) {
        if (entry.id == name) {
            return &entry;
        }
    }
    for (const Entry& entry : themes_) {
        if (entry.label == name) {
            return &entry;
        }
    }
    return nullptr;
}

namespace {

QColor to_qcolor(const core::Rgba& color) {
    return QColor(color.red, color.green, color.blue, color.alpha);
}

// What the theme says about the key, or about the key it defaults to.
std::optional<core::Rgba> own_color(const core::ColorTheme& theme, const WorkbenchColorKey& key) {
    if (const auto color = theme.color(key.key)) {
        return color;
    }
    for (const WorkbenchColorKey& other : WorkbenchColors::known_keys()) {
        if (!key.fallback_key.empty() && other.key == key.fallback_key) {
            return own_color(theme, other);
        }
    }
    return std::nullopt;
}

} // namespace

// Where a theme does not set a key, VS Code's color registry has a default for the kind of theme,
// which the editor does not have. The default theme of the same kind stands in for it: Dark 2026
// for dark themes and Light 2026 for light ones, high contrast included.
WorkbenchColors::Colors ThemeStore::compose(const Entry& entry, const core::ColorTheme& theme) {
    const bool light =
        entry.kind == core::ThemeKind::light || entry.kind == core::ThemeKind::high_contrast_light;
    const Entry* base = find(light ? "Light 2026" : "Dark 2026");
    std::optional<core::ColorTheme> base_theme;
    if (base && base->id != entry.id) {
        auto loaded =
            core::ColorTheme::load(base->file, base->kind, [](const std::filesystem::path& path) {
                return platform::read_file(path);
            });
        if (loaded) {
            base_theme = std::move(*loaded);
        } else {
            logger_->log(core::LogLevel::warning, "theme",
                         "The default theme '" + base->id +
                             "' cannot be loaded: " + loaded.error().message);
        }
    }

    WorkbenchColors::Colors table;
    for (const WorkbenchColorKey& key : WorkbenchColors::known_keys()) {
        auto color = own_color(theme, key);
        if (!color && base_theme) {
            color = own_color(*base_theme, key);
        }
        if (color) {
            table.emplace(std::string(key.key), to_qcolor(*color));
        }
    }
    return table;
}

bool ThemeStore::apply(const Entry& entry, std::string* problem) {
    if (entry.id == current_) {
        return true;
    }
    auto loaded =
        core::ColorTheme::load(entry.file, entry.kind, [](const std::filesystem::path& path) {
            return platform::read_file(path);
        });
    if (!loaded) {
        *problem = "The color theme '" + entry.id + "' cannot be loaded: " + loaded.error().message;
        return false;
    }
    for (const std::string& warning : loaded->warnings()) {
        logger_->log(core::LogLevel::warning, "theme", warning);
    }

    colors_.set_colors(compose(entry, *loaded));
    current_ = entry.id;
    theme_ = std::move(*loaded);
    last_problem_.clear();
    emit changed();
    return true;
}

bool ThemeStore::select(std::string_view name, std::string_view fallback) {
    std::string problem;
    if (const Entry* entry = find(name)) {
        if (apply(*entry, &problem)) {
            return true;
        }
    } else if (themes_.empty()) {
        problem = "There are no color themes to choose from";
    } else {
        problem = "There is no color theme named '" + std::string(name) + "'";
    }
    report(problem);

    if (current_.empty() && !fallback.empty() && fallback != name) {
        if (const Entry* entry = find(fallback)) {
            std::string ignored;
            apply(*entry, &ignored);
        }
    }
    return false;
}

void ThemeStore::report(const std::string& problem) {
    logger_->log(core::LogLevel::error, "theme", problem);
    if (problem == last_problem_) {
        return;
    }
    last_problem_ = problem;
    emit load_failed(QString::fromStdString(problem));
}

} // namespace anvil::ui
