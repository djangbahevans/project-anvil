#include "anvil/ui/settings_store.hpp"

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
        auto bytes = platform::read_file(options_.file);
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
    auto editor = core::EditorSettings::from_settings(*parsed, options_.defaults);
    if (!editor) {
        report(editor.error().message);
        return;
    }

    last_problem_.clear();
    const bool differs = parsed->root() != settings_.root() || *editor != editor_;
    settings_ = std::move(*parsed);
    editor_ = std::move(*editor);
    if (differs && notify) {
        emit changed();
    }
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
