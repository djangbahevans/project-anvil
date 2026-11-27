#include "anvil/ui/open_files_watcher.hpp"

#include "anvil/ui/watched_file.hpp"

#include <QSet>
#include <QStringList>

namespace anvil::ui {
namespace {

QString to_qstring(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

} // namespace

OpenFilesWatcher::OpenFilesWatcher(std::chrono::milliseconds delay, QObject* parent)
    : QObject(parent), watcher_(this), delay_(this) {
    delay_.setSingleShot(true);
    delay_.setInterval(static_cast<int>(delay.count()));
    connect(&delay_, &QTimer::timeout, this, &OpenFilesWatcher::report);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this,
            [this](const QString& path) { note(path.toStdString()); });
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
            [this](const QString& path) { directory_event(path.toStdString()); });
}

OpenFilesWatcher::State OpenFilesWatcher::state_of(const std::filesystem::path& file) {
    State state;
    std::error_code error;
    state.exists = std::filesystem::is_regular_file(file, error);
    if (state.exists) {
        state.modified = std::filesystem::last_write_time(file, error);
        state.size = std::filesystem::file_size(file, error);
    }
    return state;
}

void OpenFilesWatcher::set_files(const std::set<std::filesystem::path>& files) {
    std::erase_if(files_, [&](const auto& entry) { return !files.contains(entry.first); });
    std::erase_if(pending_, [&](const auto& file) { return !files.contains(file); });
    for (const std::filesystem::path& file : files) {
        if (!files_.contains(file)) {
            files_[file].state = state_of(file);
        }
    }
    watch();
}

void OpenFilesWatcher::watch() {
    QSet<QString> wanted;
    for (auto& [path, file] : files_) {
        if (file.state.exists) {
            wanted.insert(to_qstring(path));
        }
        file.directory = nearest_existing_directory(path);
        if (!file.directory.empty()) {
            wanted.insert(to_qstring(file.directory));
        }
    }
    QStringList current = watcher_.files();
    current << watcher_.directories();
    QStringList unwanted;
    for (const QString& path : current) {
        if (!wanted.remove(path)) {
            unwanted << path;
        }
    }
    if (!unwanted.isEmpty()) {
        watcher_.removePaths(unwanted);
    }
    if (!wanted.isEmpty()) {
        watcher_.addPaths({wanted.begin(), wanted.end()});
    }
}

void OpenFilesWatcher::note(const std::filesystem::path& file) {
    if (!files_.contains(file)) {
        return;
    }
    pending_.insert(file);
    if (!delay_.isActive()) {
        delay_.start();
    }
}

// A directory tells of a file created, removed or renamed over, and of nothing about the others.
void OpenFilesWatcher::directory_event(const std::filesystem::path& directory) {
    bool moved = false;
    for (const auto& [path, file] : files_) {
        if (file.directory != directory) {
            continue;
        }
        if (!(state_of(path) == file.state)) {
            note(path);
        }
        moved = moved || nearest_existing_directory(path) != file.directory;
    }
    // A directory on the way to a missing file appeared or went away.
    if (moved) {
        watch();
    }
}

// A rename over a watched file leaves the watch on the replaced file, so the watches of the files
// reported are set again, before anyone reads them.
void OpenFilesWatcher::report() {
    const std::vector<std::filesystem::path> files(pending_.begin(), pending_.end());
    pending_.clear();
    const QStringList watched = watcher_.files();
    for (const std::filesystem::path& file : files) {
        files_[file].state = state_of(file);
        if (const QString path = to_qstring(file); watched.contains(path)) {
            watcher_.removePath(path);
        }
    }
    watch();
    emit changed(files);
}

} // namespace anvil::ui
