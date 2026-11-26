#include "anvil/platform/process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace anvil::platform {
namespace {

Error errno_error(const std::string& what) {
    return Error{ErrorCode::io_error, what + ": " + std::strerror(errno)};
}

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Fd() {
        reset();
    }

    int get() const {
        return fd_;
    }
    explicit operator bool() const {
        return fd_ >= 0;
    }
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

struct Pipe {
    Fd read;
    Fd write;
};

// Both ends are closed in every child that does not get one as a standard stream: a child that
// held the write end of another child's input would keep that input open.
std::expected<Pipe, Error> make_pipe() {
    std::array<int, 2> ends{};
#ifdef __APPLE__
    if (::pipe(ends.data()) < 0) {
        return std::unexpected(errno_error("pipe"));
    }
    Pipe pipe{Fd(ends[0]), Fd(ends[1])};
    if (::fcntl(ends[0], F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(ends[1], F_SETFD, FD_CLOEXEC) < 0) {
        return std::unexpected(errno_error("fcntl(FD_CLOEXEC)"));
    }
    return pipe;
#else
    if (::pipe2(ends.data(), O_CLOEXEC) < 0) {
        return std::unexpected(errno_error("pipe2"));
    }
    return Pipe{Fd(ends[0]), Fd(ends[1])};
#endif
}

// The pipes of a child that is being started: the ends that it gets, and ours.
struct Streams {
    Fd child_input;
    Fd child_output;
    Fd child_error;
    Fd input;
    Fd output;
    Fd error;
};

// A descriptor above the standard ones, so that putting the pipes in place in the child cannot
// overwrite one that is still to be used, as it could when we run with a standard stream closed.
std::expected<Fd, Error> above_standard_streams(Fd fd) {
    if (fd.get() > STDERR_FILENO) {
        return fd;
    }
    Fd moved(::fcntl(fd.get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1));
    if (!moved) {
        return std::unexpected(errno_error("fcntl(F_DUPFD_CLOEXEC)"));
    }
    return moved;
}

std::expected<void, Error> add_stream(bool child_reads, Fd& child_end, Fd& our_end) {
    auto pipe = make_pipe();
    if (!pipe) {
        return std::unexpected(pipe.error());
    }
    auto theirs = above_standard_streams(std::move(child_reads ? pipe->read : pipe->write));
    if (!theirs) {
        return std::unexpected(theirs.error());
    }
    auto ours = above_standard_streams(std::move(child_reads ? pipe->write : pipe->read));
    if (!ours) {
        return std::unexpected(ours.error());
    }
    child_end = std::move(*theirs);
    our_end = std::move(*ours);
    return {};
}

std::expected<Streams, Error> make_streams(const ProcessSpec& spec) {
    Streams streams;
    if (spec.pipe_input) {
        if (auto added = add_stream(true, streams.child_input, streams.input); !added) {
            return std::unexpected(added.error());
        }
        // A write takes what there is room for and returns, so that it can have a timeout.
        if (::fcntl(streams.input.get(), F_SETFL, O_NONBLOCK) < 0) {
            return std::unexpected(errno_error("fcntl(O_NONBLOCK)"));
        }
#ifdef __APPLE__
        if (::fcntl(streams.input.get(), F_SETNOSIGPIPE, 1) < 0) {
            return std::unexpected(errno_error("fcntl(F_SETNOSIGPIPE)"));
        }
#endif
    }
    if (spec.pipe_output) {
        if (auto added = add_stream(false, streams.child_output, streams.output); !added) {
            return std::unexpected(added.error());
        }
    }
    if (spec.pipe_error) {
        if (auto added = add_stream(false, streams.child_error, streams.error); !added) {
            return std::unexpected(added.error());
        }
    }
    return streams;
}

// Waits for events on the descriptors. False on a timeout, also when a signal ends the wait.
std::expected<bool, Error> wait_for(std::span<pollfd> descriptors,
                                    std::chrono::milliseconds timeout) {
    const auto milliseconds = std::clamp<std::int64_t>(timeout.count(), 0, INT_MAX);
    const int result = ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()),
                              static_cast<int>(milliseconds));
    if (result < 0) {
        if (errno == EINTR) {
            return false;
        }
        return std::unexpected(errno_error("poll"));
    }
    return result > 0;
}

// `write` that fails with EPIPE where a pipe without a reader would otherwise end this process
// with SIGPIPE. The signal is blocked in this thread for the write and the one it raised is taken
// back, so that what the rest of the process, and its children, do with SIGPIPE stays as it was.
ssize_t write_without_sigpipe(int fd, std::span<const std::byte> data) {
#ifdef __APPLE__
    return ::write(fd, data.data(), data.size());
#else
    sigset_t pipe_signal;
    sigemptyset(&pipe_signal);
    sigaddset(&pipe_signal, SIGPIPE);
    sigset_t pending;
    sigpending(&pending);
    const bool was_pending = sigismember(&pending, SIGPIPE) == 1;
    sigset_t previous;
    pthread_sigmask(SIG_BLOCK, &pipe_signal, &previous);
    const ssize_t count = ::write(fd, data.data(), data.size());
    const int error = errno;
    if (count < 0 && error == EPIPE && !was_pending) {
        const timespec at_once{};
        sigtimedwait(&pipe_signal, nullptr, &at_once);
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = error;
    return count;
#endif
}

// A piped stream of the child's that we read.
struct Reader {
    Fd fd;
    bool ended = false;

    std::expected<ReadResult, Error> read(std::span<std::byte> buffer,
                                          std::chrono::milliseconds timeout) {
        if (!fd) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "the stream of the child is not a pipe"});
        }
        std::array<pollfd, 1> descriptors{{{fd.get(), POLLIN, 0}}};
        const auto ready = wait_for(descriptors, timeout);
        if (!ready) {
            return std::unexpected(ready.error());
        }
        if (!*ready) {
            return ReadResult{};
        }
        const ssize_t count = ::read(fd.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                return ReadResult{};
            }
            return std::unexpected(errno_error("read"));
        }
        ended = count == 0 && !buffer.empty();
        return ReadResult{static_cast<std::size_t>(count), ended};
    }
};

} // namespace

struct Process::Impl {
    pid_t pid = -1;
    std::optional<int> exit_code;
    Fd input;
    Reader output;
    Reader error;

    ~Impl() {
        if (pid > 0 && !exit_code) {
            ::kill(pid, SIGKILL);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    void poll() {
        if (exit_code || pid <= 0) {
            return;
        }
        int status = 0;
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result != pid) {
            return;
        }
        if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_code = 128 + WTERMSIG(status);
        }
    }
};

namespace {

std::expected<pid_t, Error> start(const ProcessSpec& spec, bool own_session,
                                  const Streams& streams) {
    const std::string executable = spec.executable.string();

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : spec.arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    std::vector<char*> envp;
    for (const auto& entry : spec.environment) {
        envp.push_back(const_cast<char*>(entry.c_str()));
    }
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (streams.child_input) {
        posix_spawn_file_actions_adddup2(&actions, streams.child_input.get(), STDIN_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    if (streams.child_output) {
        posix_spawn_file_actions_adddup2(&actions, streams.child_output.get(), STDOUT_FILENO);
    }
    if (streams.child_error) {
        posix_spawn_file_actions_adddup2(&actions, streams.child_error.get(), STDERR_FILENO);
    }
    const std::string directory =
        spec.working_directory ? spec.working_directory->string() : std::string();
    if (spec.working_directory) {
        // posix_spawn reports a failure to enter the directory as the same errno as a missing
        // executable, so a directory that is plainly absent is named before trying.
        std::error_code ignored;
        if (!std::filesystem::is_directory(*spec.working_directory, ignored)) {
            posix_spawn_file_actions_destroy(&actions);
            return std::unexpected(Error{ErrorCode::io_error,
                                         "cannot start " + executable + ": the working directory " +
                                             directory + " is not a directory"});
        }
        if (const int added = posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
            added != 0) {
            posix_spawn_file_actions_destroy(&actions);
            return std::unexpected(Error{ErrorCode::io_error, "cannot start " + executable +
                                                                  " in " + directory + ": " +
                                                                  std::strerror(added)});
        }
    }

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    if (own_session) {
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    }
    pid_t pid = -1;
    const int result =
        posix_spawn(&pid, executable.c_str(), &actions, &attributes, argv.data(), envp.data());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        return std::unexpected(
            Error{ErrorCode::io_error, "cannot start " + executable +
                                           (directory.empty() ? "" : " in " + directory) + ": " +
                                           std::strerror(result)});
    }
    return pid;
}

} // namespace

std::expected<Process, Error> Process::spawn(const ProcessSpec& spec) {
    auto streams = make_streams(spec);
    if (!streams) {
        return std::unexpected(streams.error());
    }
    auto impl = std::make_unique<Impl>();
    const auto pid = start(spec, false, *streams);
    if (!pid) {
        return std::unexpected(pid.error());
    }
    impl->pid = *pid;
    impl->input = std::move(streams->input);
    impl->output.fd = std::move(streams->output);
    impl->error.fd = std::move(streams->error);
    return Process(std::move(impl));
}

std::expected<std::int64_t, Error> spawn_detached(const ProcessSpec& spec) {
    if (spec.pipe_input || spec.pipe_output || spec.pipe_error) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
                                     "a process that goes on after this one cannot have pipes"});
    }
    const auto pid = start(spec, true, Streams{});
    if (!pid) {
        return std::unexpected(pid.error());
    }
    std::thread([child = *pid] {
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return *pid;
}

Process::Process(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;
Process::~Process() = default;

std::int64_t Process::id() const {
    return impl_->pid;
}

std::optional<int> Process::wait(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    impl_->poll();
    while (!impl_->exit_code && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        impl_->poll();
    }
    return impl_->exit_code;
}

std::expected<std::size_t, Error> Process::write_input(std::span<const std::byte> data,
                                                       std::chrono::milliseconds timeout) {
    if (!impl_->input) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "the input of the child is not an open pipe"});
    }
    std::array<pollfd, 1> descriptors{{{impl_->input.get(), POLLOUT, 0}}};
    const auto ready = wait_for(descriptors, timeout);
    if (!ready) {
        return std::unexpected(ready.error());
    }
    if (!*ready) {
        return std::size_t{0};
    }
    const ssize_t count = write_without_sigpipe(impl_->input.get(), data);
    if (count < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return std::size_t{0};
        }
        return std::unexpected(errno_error("write to the child"));
    }
    return static_cast<std::size_t>(count);
}

void Process::close_input() {
    impl_->input.reset();
}

std::expected<ReadResult, Error> Process::read_output(std::span<std::byte> buffer,
                                                      std::chrono::milliseconds timeout) {
    return impl_->output.read(buffer, timeout);
}

std::expected<ReadResult, Error> Process::read_error(std::span<std::byte> buffer,
                                                     std::chrono::milliseconds timeout) {
    return impl_->error.read(buffer, timeout);
}

std::expected<bool, Error> Process::wait_for_output(std::chrono::milliseconds timeout) {
    std::array<pollfd, 2> descriptors{};
    std::size_t count = 0;
    for (const Reader* reader : {&impl_->output, &impl_->error}) {
        if (reader->fd && !reader->ended) {
            descriptors[count++] = {reader->fd.get(), POLLIN, 0};
        }
    }
    if (count == 0) {
        return false;
    }
    return wait_for(std::span(descriptors).first(count), timeout);
}

void Process::terminate() {
    if (impl_->pid > 0 && !impl_->exit_code) {
        ::kill(impl_->pid, SIGTERM);
    }
}

void Process::kill() {
    if (impl_->pid > 0 && !impl_->exit_code) {
        ::kill(impl_->pid, SIGKILL);
    }
}

std::int64_t current_process_id() {
    return ::getpid();
}

std::vector<std::string> current_environment() {
    std::vector<std::string> result;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        result.emplace_back(*entry);
    }
    return result;
}

} // namespace anvil::platform
