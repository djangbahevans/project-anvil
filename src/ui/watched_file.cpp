#include "anvil/ui/watched_file.hpp"

#include <QStringList>

namespace anvil::ui {
namespace {

QString to_qstring(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

} // namespace

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

WatchedFile::WatchedFile(std::filesystem::path file, std::chrono::milliseconds debounce,
                         QObject* parent)
    : QObject(parent), file_(std::move(file)), watcher_(this), debounce_(this) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(static_cast<int>(debounce.count()));
    connect(&debounce_, &QTimer::timeout, this, [this] {
        watch();
        emit modified();
    });
    const auto schedule = [this] { debounce_.start(); };
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, schedule);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, schedule);
}

void WatchedFile::start() {
    watch();
}

// A rename over the watched file leaves the watch on the replaced file, so the paths are set again
// after every change.
void WatchedFile::watch() {
    QStringList wanted;
    std::error_code ignored;
    if (std::filesystem::exists(file_, ignored)) {
        wanted << to_qstring(file_);
    }
    if (const auto directory = nearest_existing_directory(file_); !directory.empty()) {
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

} // namespace anvil::ui
