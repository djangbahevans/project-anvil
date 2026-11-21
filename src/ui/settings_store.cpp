#include "anvil/ui/settings_store.hpp"

#include "anvil/platform/file.hpp"

#include <QDir>
#include <QStringList>

namespace anvil::ui {
namespace {

QString to_qstring(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

// The deepest directory that exists on the way to `file`, which is where a file that does not
// exist yet will first show up.
std::filesystem::path nearest_existing_directory(const std::filesystem::path& file) {
    std::error_code ignored;
    std::filesystem::path directory = file.parent_path();
    while (!directory.empty() && !std::filesystem::is_directory(directory, ignored)) {
        const std::filesystem::path parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return directory;
}

} // namespace

SettingsStore::SettingsStore(Options options, QObject* parent)
    : QObject(parent), options_(std::move(options)), watcher_(this), debounce_(this) {
    if (!options_.logger) {
        options_.logger = std::make_shared<core::NullLogger>();
    }
    editor_ = *core::EditorSettings::from_settings(settings_, options_.defaults);
    debounce_.setSingleShot(true);
    debounce_.setInterval(static_cast<int>(options_.debounce.count()));
    connect(&debounce_, &QTimer::timeout, this, [this] {
        watch();
        reload(true);
    });
    const auto schedule = [this] { debounce_.start(); };
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, schedule);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, schedule);
}

void SettingsStore::start() {
    if (options_.file.empty()) {
        return;
    }
    reload(false);
    watch();
}

// An editor that saves by writing a temporary file and renaming it over the original leaves the
// watch on the replaced file, so the watched paths are set again after every change.
void SettingsStore::watch() {
    QStringList wanted;
    std::error_code ignored;
    if (std::filesystem::exists(options_.file, ignored)) {
        wanted << to_qstring(options_.file);
    }
    if (const auto directory = nearest_existing_directory(options_.file); !directory.empty()) {
        wanted << to_qstring(directory);
    }
    QStringList current = watcher_.files();
    current << watcher_.directories();
    if (!current.isEmpty()) {
        watcher_.removePaths(current);
    }
    if (!wanted.isEmpty()) {
        watcher_.addPaths(wanted);
    }
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
