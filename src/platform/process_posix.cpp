#include "anvil/platform/process.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace anvil::platform {

struct Process::Impl {
    pid_t pid = -1;
    std::optional<int> exit_code;

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

std::expected<pid_t, Error> start(const ProcessSpec& spec, bool own_session) {
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
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
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
    const auto pid = start(spec, false);
    if (!pid) {
        return std::unexpected(pid.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->pid = *pid;
    return Process(std::move(impl));
}

std::expected<std::int64_t, Error> spawn_detached(const ProcessSpec& spec) {
    const auto pid = start(spec, true);
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
