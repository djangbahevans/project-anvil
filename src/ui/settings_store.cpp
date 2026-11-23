#include "anvil/ui/settings_store.hpp"

#include "anvil/core/jsonc_edit.hpp"
#include "anvil/platform/file.hpp"

namespace anvil::ui {
SettingsStore::SettingsStore(Options options, QObject* parent)
    : QObject(parent), options_(std::move(options)), file_(options_.file, options_.debounce, this) {
    if (!options_.logger) {
        options_.logger = std::make_shared<core::NullLogger>();
    }
    editor_ = *core::EditorSettings::from_settings(settings_, options_.defaults);
    connect(&file_, &WatchedFile::modified, this, [this] { reload(true); });
}

void SettingsStore::start() {
    if (options_.file.empty()) {
        return;
    }
    reload(false);
    file_.start();
}

void SettingsStore::reload(bool notify) {
    std::string text;
    std::error_code ignored;
    if (std::filesystem::exists(options_.file, ignored)) {
        auto bytes = read(options_.file);
        if (!bytes) {
            report(bytes.error().message);
            return;
        }
        text = std::move(*bytes);
    }

    auto parsed = core::Settings::parse(text);
    if (!parsed) {
        report(parsed.error().message);
        return;
    }
    auto effective = with_overrides(*parsed, overrides_);
    if (!effective) {
        report(effective.error());
        return;
    }

    last_problem_.clear();
    const bool differs =
        effective->settings.root() != settings_.root() || effective->editor != editor_;
    loaded_ = std::move(*parsed);
    settings_ = std::move(effective->settings);
    editor_ = std::move(effective->editor);
    if (differs && notify) {
        emit changed();
    }
}

std::expected<std::string, Error> SettingsStore::read(const std::filesystem::path& path) const {
    return options_.read_file ? options_.read_file(path) : platform::read_file(path);
}

std::expected<SettingsStore::Effective, std::string>
SettingsStore::with_overrides(const core::Settings& loaded,
                              const std::map<std::string, nlohmann::json>& overrides) const {
    nlohmann::json root = loaded.root();
    // A flat key at the top level is the one that `Settings::find` reads before any nested
    // spelling of it.
    for (const auto& [key, value] : overrides) {
        root[key] = value;
    }
    auto settings = core::Settings::from_json(std::move(root));
    if (!settings) {
        return std::unexpected(settings.error().message);
    }
    auto editor = core::EditorSettings::from_settings(*settings, options_.defaults);
    if (!editor) {
        return std::unexpected(editor.error().message);
    }
    return Effective{std::move(*settings), std::move(*editor)};
}

void SettingsStore::adopt(core::Settings loaded, Effective effective) {
    const bool differs =
        effective.settings.root() != settings_.root() || effective.editor != editor_;
    loaded_ = std::move(loaded);
    settings_ = std::move(effective.settings);
    editor_ = std::move(effective.editor);
    last_problem_.clear();
    if (differs) {
        emit changed();
    }
}

SettingsStore::ChangeResult SettingsStore::change(std::string_view key, nlohmann::json value) {
    const std::string name(key);
    const auto reason = write_change(name, value);
    if (!reason) {
        return {ChangeResult::Outcome::written, {}};
    }

    auto overrides = overrides_;
    overrides[name] = std::move(value);
    auto effective = with_overrides(loaded_, overrides);
    if (!effective) {
        return {ChangeResult::Outcome::rejected, name + ": " + effective.error()};
    }
    overrides_ = std::move(overrides);
    core::Settings loaded = loaded_;
    adopt(std::move(loaded), std::move(*effective));
    return {ChangeResult::Outcome::session, *reason};
}

std::optional<std::string> SettingsStore::write_change(const std::string& key,
                                                       const nlohmann::json& value) {
    if (options_.file.empty()) {
        return "there is no settings file";
    }
    const std::string name = options_.file.filename().string();
    if (options_.is_unsaved && options_.is_unsaved(options_.file)) {
        return name + " has unsaved changes in an editor";
    }

    const auto read_text = [&]() -> std::expected<std::string, Error> {
        std::error_code ignored;
        if (!std::filesystem::exists(options_.file, ignored)) {
            return std::string();
        }
        return read(options_.file);
    };
    const auto text = read_text();
    if (!text) {
        return name + " cannot be read: " + text.error().message;
    }
    const auto edited = core::edit_jsonc_property(*text, key, value);
    if (!edited) {
        return name + ": " + edited.error().message;
    }
    auto parsed = core::Settings::parse(*edited);
    if (!parsed) {
        return name + ": " + parsed.error().message;
    }
    auto overrides = overrides_;
    overrides.erase(key);
    auto effective = with_overrides(*parsed, overrides);
    if (!effective) {
        return name + ": " + effective.error();
    }

    const auto again = read_text();
    if (!again || *again != *text) {
        return name + " changed while it was being edited";
    }
    std::error_code ignored;
    std::filesystem::create_directories(options_.file.parent_path(), ignored);
    if (const auto written = platform::write_file(options_.file, *edited); !written) {
        return name + " cannot be written: " + written.error().message;
    }

    overrides_ = std::move(overrides);
    adopt(std::move(*parsed), std::move(*effective));
    return std::nullopt;
}

void SettingsStore::report(const std::string& problem) {
    if (problem == last_problem_) {
        return;
    }
    last_problem_ = problem;
    const std::string message = options_.file.filename().string() + ": " + problem;
    options_.logger->log(core::LogLevel::error, "settings",
                         options_.file.string() + ": " + problem);
    emit load_failed(QString::fromStdString(message));
}

} // namespace anvil::ui
