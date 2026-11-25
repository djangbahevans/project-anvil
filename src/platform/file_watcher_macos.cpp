#include "anvil/platform/file_watcher.hpp"

namespace anvil::platform {

// Not built or run yet: the macos preset has not been built. Watching needs FSEvents; until then a
// watch does not start.
struct FileWatcher::Impl {};

FileWatcher::FileWatcher(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FileWatcher::~FileWatcher() = default;

std::expected<std::unique_ptr<FileWatcher>, Error> FileWatcher::start(const std::filesystem::path&,
                                                                      const WatchOptions&) {
    return std::unexpected(
        Error{ErrorCode::unsupported, "watching files is not supported on this platform yet"});
}

std::expected<std::vector<FileEvent>, Error> FileWatcher::poll(std::chrono::milliseconds) {
    return std::vector<FileEvent>{};
}

std::expected<void, Error> FileWatcher::add_directory(const std::filesystem::path&) {
    return std::unexpected(
        Error{ErrorCode::unsupported, "watching files is not supported on this platform yet"});
}

void FileWatcher::remove_directory(const std::filesystem::path&) {}

void FileWatcher::stop() {}

} // namespace anvil::platform
