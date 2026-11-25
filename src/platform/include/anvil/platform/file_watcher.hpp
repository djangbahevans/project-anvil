#pragma once

#include "anvil/core/error.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace anvil::platform {

enum class FileEventKind { created, changed, deleted };

struct FileEvent {
    FileEventKind kind = FileEventKind::changed;
    // Absolute, below the watched path.
    std::filesystem::path path;

    friend bool operator==(const FileEvent&, const FileEvent&) = default;
};

struct WatchOptions {
    // Watch the directories below the path too, including those that are created later.
    bool recursive = true;
    // Globs (see core::glob_match) of paths relative to the watched path. A directory that
    // matches is not watched and nothing below it is reported.
    std::vector<std::string> excludes{};
    // Events of a burst are delivered together: they wait until no event has come for this long,
    // and at most `max_delay` after the first.
    std::chrono::milliseconds latency{50};
    std::chrono::milliseconds max_delay{500};
    // Fail instead of watching more directories than this; 0 leaves the limit to the system
    // (`fs.inotify.max_user_watches` on Linux).
    std::size_t max_directories = 0;
};

// Reports the files and directories that are created, changed and deleted below a path, as the
// operating system tells. A worker thread collects the events; the owner reads them with poll()
// when its event loop decides to, and the interface has no Qt.
//
// Events of a burst for one path are merged into one: a created path that changes stays created,
// a created path that is deleted is not reported, a changed path that is deleted is deleted, and a
// deleted path that is created again is changed. A renamed file is a deleted old path and a
// created new path. A directory that appears is reported as created together with what it holds.
// Starting a watch reports nothing for what exists.
//
// One implementation per operating system; Windows and macOS fail to start with `unsupported`.
class FileWatcher {
public:
    // Watches `path`, a directory or a file. Fails with `not_found` when it does not exist and
    // with `limit_exceeded` when the system or `max_directories` allows no more directories.
    static std::expected<std::unique_ptr<FileWatcher>, Error>
    start(const std::filesystem::path& path, const WatchOptions& options = {});

    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Waits up to `timeout` for events and takes all of them; an empty result is a timeout.
    // Returns an error that the watch ran into after its events were taken: events were lost
    // because the system's queue overflowed, or a directory could not be watched. The watch goes
    // on, and the error is returned once.
    std::expected<std::vector<FileEvent>, Error> poll(std::chrono::milliseconds timeout);

    // Watches one more directory, without the directories below it, together with the ones that
    // are watched already: the events of all of them come out of poll() with the other events. For
    // a watcher that is not recursive and was started on a directory; `directory` is that
    // directory or below it. Watching a directory again changes nothing. Fails with `not_found`
    // when the directory does not exist, `limit_exceeded` as `start` does, and `invalid_argument`
    // for a recursive watcher, a watcher of a file, or a directory outside the watched one.
    // Windows and macOS fail with `unsupported`.
    std::expected<void, Error> add_directory(const std::filesystem::path& directory);

    // Stops watching a directory that `add_directory` added. The watched path itself stays, and so
    // does what was collected already.
    void remove_directory(const std::filesystem::path& directory);

    // Ends the watch; later polls return nothing. Also done by the destructor.
    void stop();

private:
    struct Impl;
    explicit FileWatcher(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace anvil::platform
