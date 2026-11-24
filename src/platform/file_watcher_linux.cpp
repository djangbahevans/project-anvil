#include "anvil/core/glob.hpp"
#include "anvil/platform/file_watcher.hpp"

#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace anvil::platform {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t directory_mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB |
                                         IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                                         IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW;
constexpr std::uint32_t file_mask = IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;

// The events of a burst, one per path, in the order the paths first appeared.
class Pending {
public:
    bool empty() const {
        return live_ == 0;
    }

    void add(FileEventKind kind, const fs::path& path) {
        const std::string key = path.string();
        const auto found = index_.find(key);
        if (found == index_.end()) {
            index_.emplace(key, events_.size());
            events_.push_back(FileEvent{kind, path});
            ++live_;
            return;
        }
        std::optional<FileEvent>& slot = events_[found->second];
        if (!slot) {
            slot = FileEvent{kind, path};
            ++live_;
            return;
        }
        using Kind = FileEventKind;
        const Kind before = slot->kind;
        if (before == Kind::created && kind == Kind::deleted) {
            slot.reset();
            --live_;
        } else if (before == Kind::changed && kind == Kind::deleted) {
            slot->kind = Kind::deleted;
        } else if (before == Kind::deleted && kind == Kind::created) {
            slot->kind = Kind::changed;
        }
    }

    std::vector<FileEvent> take() {
        std::vector<FileEvent> result;
        for (std::optional<FileEvent>& slot : events_) {
            if (slot) {
                result.push_back(std::move(*slot));
            }
        }
        events_.clear();
        index_.clear();
        live_ = 0;
        return result;
    }

private:
    std::vector<std::optional<FileEvent>> events_;
    std::unordered_map<std::string, std::size_t> index_;
    std::size_t live_ = 0;
};

Error errno_error(const std::string& what, int code) {
    return Error{ErrorCode::io_error, what + ": " + std::strerror(code)};
}

} // namespace

struct FileWatcher::Impl {
    fs::path root;
    bool root_is_file = false;
    WatchOptions options;
    int inotify_fd = -1;
    int wake_fd = -1;
    int root_wd = -1;
    // The watched directories. Written by start() before the worker runs and by the worker after.
    std::unordered_map<int, fs::path> directories;
    std::thread worker;

    std::mutex mutex;
    std::condition_variable ready;
    Pending pending;
    Clock::time_point first_at;
    Clock::time_point last_at;
    std::optional<Error> error;
    bool stopped = false;

    ~Impl() {
        if (inotify_fd >= 0) {
            ::close(inotify_fd);
        }
        if (wake_fd >= 0) {
            ::close(wake_fd);
        }
    }

    bool excluded(const fs::path& path) const {
        if (options.excludes.empty()) {
            return false;
        }
        const std::string relative = path.lexically_relative(root).generic_string();
        for (const std::string& pattern : options.excludes) {
            if (core::glob_match(pattern, relative)) {
                return true;
            }
        }
        return false;
    }

    void report(FileEventKind kind, const fs::path& path) {
        const std::lock_guard lock(mutex);
        const Clock::time_point now = Clock::now();
        if (pending.empty()) {
            first_at = now;
        }
        pending.add(kind, path);
        last_at = now;
        ready.notify_all();
    }

    void fail(Error problem) {
        const std::lock_guard lock(mutex);
        if (!error) {
            error = std::move(problem);
        }
        ready.notify_all();
    }

    // Watches the directory; false when it is gone already.
    std::expected<bool, Error> watch_directory(const fs::path& path) {
        if (options.max_directories != 0 && directories.size() >= options.max_directories) {
            return std::unexpected(Error{ErrorCode::limit_exceeded,
                                         "more than " + std::to_string(options.max_directories) +
                                             " directories to watch"});
        }
        const int wd = ::inotify_add_watch(inotify_fd, path.c_str(), directory_mask);
        if (wd >= 0) {
            directories[wd] = path;
            return true;
        }
        if (errno == ENOENT || errno == ENOTDIR) {
            return false;
        }
        if (errno == ENOSPC) {
            return std::unexpected(Error{ErrorCode::limit_exceeded,
                                         "the system limit of watched directories is reached "
                                         "(fs.inotify.max_user_watches), at " +
                                             path.string()});
        }
        return std::unexpected(errno_error("cannot watch " + path.string(), errno));
    }

    // Watches the directories below `directory` and, with `announce`, reports what is in it.
    std::expected<void, Error> scan(const fs::path& directory, bool announce) {
        std::error_code ec;
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path child = it->path();
            if (excluded(child)) {
                continue;
            }
            if (announce) {
                report(FileEventKind::created, child);
            }
            std::error_code type_ec;
            if (!fs::is_directory(fs::symlink_status(child, type_ec))) {
                continue;
            }
            const auto watched = watch_directory(child);
            if (!watched) {
                return std::unexpected(watched.error());
            }
            if (*watched) {
                if (auto scanned = scan(child, announce); !scanned) {
                    return scanned;
                }
            }
        }
        return {};
    }

    void forget_below(const fs::path& directory) {
        for (auto it = directories.begin(); it != directories.end();) {
            const bool inside = it->second == directory ||
                                (it->second.string().size() > directory.string().size() &&
                                 it->second.string().starts_with(directory.string() + "/"));
            if (inside) {
                ::inotify_rm_watch(inotify_fd, it->first);
                it = directories.erase(it);
            } else {
                ++it;
            }
        }
    }

    void handle(const inotify_event& event, const std::string& name) {
        if (event.mask & IN_Q_OVERFLOW) {
            fail(Error{ErrorCode::limit_exceeded,
                       "the system dropped file events because its queue overflowed"});
            return;
        }
        if (event.mask & IN_IGNORED) {
            directories.erase(event.wd);
            return;
        }
        const auto found = directories.find(event.wd);
        if (found == directories.end()) {
            return;
        }
        const fs::path directory = found->second;
        const bool is_directory = (event.mask & IN_ISDIR) != 0;

        if (event.wd == root_wd && (event.mask & (IN_DELETE_SELF | IN_MOVE_SELF))) {
            report(FileEventKind::deleted, root);
            forget_below(root);
            return;
        }
        if (root_is_file) {
            if (event.mask & (IN_MODIFY | IN_ATTRIB)) {
                report(FileEventKind::changed, root);
            }
            return;
        }
        if (name.empty()) {
            return;
        }
        const fs::path path = directory / name;
        if (excluded(path)) {
            return;
        }
        if (event.mask & (IN_CREATE | IN_MOVED_TO)) {
            report(FileEventKind::created, path);
            if (is_directory && options.recursive) {
                const auto watched = watch_directory(path);
                if (!watched) {
                    fail(watched.error());
                } else if (*watched) {
                    if (auto scanned = scan(path, true); !scanned) {
                        fail(scanned.error());
                    }
                }
            }
        } else if (event.mask & (IN_DELETE | IN_MOVED_FROM)) {
            report(FileEventKind::deleted, path);
            if (is_directory) {
                forget_below(path);
            }
        } else if ((event.mask & (IN_MODIFY | IN_ATTRIB)) && !is_directory) {
            report(FileEventKind::changed, path);
        }
    }

    void run() {
        std::array<char, 64 * 1024> buffer;
        for (;;) {
            std::array<pollfd, 2> fds{{{inotify_fd, POLLIN, 0}, {wake_fd, POLLIN, 0}}};
            if (::poll(fds.data(), fds.size(), -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fail(errno_error("waiting for file events", errno));
                return;
            }
            if (fds[1].revents != 0) {
                return;
            }
            const ssize_t count = ::read(inotify_fd, buffer.data(), buffer.size());
            if (count < 0) {
                if (errno != EAGAIN && errno != EINTR) {
                    fail(errno_error("reading file events", errno));
                    return;
                }
                continue;
            }
            for (ssize_t offset = 0; offset < count;) {
                const char* record = buffer.data() + offset;
                inotify_event event;
                std::memcpy(&event, record, sizeof event);
                std::string name(record + offsetof(inotify_event, name), event.len);
                name.resize(std::strlen(name.c_str()));
                handle(event, name);
                offset += static_cast<ssize_t>(sizeof(inotify_event) + event.len);
            }
        }
    }
};

FileWatcher::FileWatcher(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FileWatcher::~FileWatcher() {
    stop();
}

std::expected<std::unique_ptr<FileWatcher>, Error> FileWatcher::start(const fs::path& path,
                                                                      const WatchOptions& options) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    if (ec || !fs::exists(absolute, ec)) {
        return std::unexpected(Error{ErrorCode::not_found, path.string() + " does not exist"});
    }
    auto impl = std::make_unique<Impl>();
    impl->root = absolute.lexically_normal();
    if (!impl->root.has_filename()) {
        impl->root = impl->root.parent_path();
    }
    impl->options = options;
    impl->root_is_file = !fs::is_directory(impl->root, ec);
    impl->inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    impl->wake_fd = ::eventfd(0, EFD_CLOEXEC);
    if (impl->inotify_fd < 0 || impl->wake_fd < 0) {
        return std::unexpected(errno_error("cannot watch files", errno));
    }
    if (impl->root_is_file) {
        impl->root_wd = ::inotify_add_watch(impl->inotify_fd, impl->root.c_str(), file_mask);
        if (impl->root_wd < 0) {
            return std::unexpected(errno_error("cannot watch " + impl->root.string(), errno));
        }
        impl->directories[impl->root_wd] = impl->root;
    } else {
        const auto watched = impl->watch_directory(impl->root);
        if (!watched) {
            return std::unexpected(watched.error());
        }
        if (!*watched) {
            return std::unexpected(Error{ErrorCode::not_found, path.string() + " does not exist"});
        }
        impl->root_wd = impl->directories.begin()->first;
        if (options.recursive) {
            if (auto scanned = impl->scan(impl->root, false); !scanned) {
                return std::unexpected(scanned.error());
            }
        }
    }
    Impl* raw = impl.get();
    raw->worker = std::thread([raw] { raw->run(); });
    return std::unique_ptr<FileWatcher>(new FileWatcher(std::move(impl)));
}

void FileWatcher::stop() {
    if (!impl_) {
        return;
    }
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->stopped) {
            return;
        }
        impl_->stopped = true;
        impl_->ready.notify_all();
    }
    const std::uint64_t one = 1;
    (void)!::write(impl_->wake_fd, &one, sizeof one);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

std::expected<std::vector<FileEvent>, Error> FileWatcher::poll(std::chrono::milliseconds timeout) {
    Impl& impl = *impl_;
    const Clock::time_point deadline = Clock::now() + timeout;
    std::unique_lock lock(impl.mutex);
    for (;;) {
        if (impl.stopped) {
            return std::vector<FileEvent>{};
        }
        const Clock::time_point now = Clock::now();
        Clock::time_point wake = deadline;
        if (!impl.pending.empty()) {
            const Clock::time_point due = std::min(impl.last_at + impl.options.latency,
                                                   impl.first_at + impl.options.max_delay);
            if (now >= due) {
                return impl.pending.take();
            }
            wake = std::min(due, deadline);
        } else if (impl.error) {
            Error problem = std::move(*impl.error);
            impl.error.reset();
            return std::unexpected(std::move(problem));
        }
        if (now >= deadline) {
            return std::vector<FileEvent>{};
        }
        impl.ready.wait_until(lock, wake);
    }
}

} // namespace anvil::platform
