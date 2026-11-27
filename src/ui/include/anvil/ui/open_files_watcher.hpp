#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <vector>

namespace anvil::ui {

// Tells which of a set of files may have changed: written in place, replaced by renaming another
// file over it, deleted or created again. One QFileSystemWatcher serves all of them, since each
// takes an inotify instance of its own, of which Linux allows a user few. It watches the files and,
// for what a watch on a file does not see, the nearest existing directory above each.
class OpenFilesWatcher : public QObject {
    Q_OBJECT

public:
    // Events are collected for `delay` from the first of them, so that a writer that never pauses
    // is still reported.
    OpenFilesWatcher(std::chrono::milliseconds delay, QObject* parent = nullptr);

    // Watches these files and no others. Nothing is reported for their state at this time.
    void set_files(const std::set<std::filesystem::path>& files);

signals:
    // Files that may no longer hold what they held. A file can be named that still does.
    void changed(const std::vector<std::filesystem::path>& files);

private:
    // What tells that a file differs without reading it.
    struct State {
        bool exists = false;
        std::filesystem::file_time_type modified{};
        std::uintmax_t size = 0;

        bool operator==(const State&) const = default;
    };
    struct File {
        State state;
        std::filesystem::path directory;
    };

    static State state_of(const std::filesystem::path& file);
    void watch();
    void note(const std::filesystem::path& file);
    void directory_event(const std::filesystem::path& directory);
    void report();

    std::map<std::filesystem::path, File> files_;
    std::set<std::filesystem::path> pending_;
    QFileSystemWatcher watcher_;
    QTimer delay_;
};

} // namespace anvil::ui
