#pragma once

#include "anvil/core/error.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anvil::platform {

struct ProcessSpec {
    std::filesystem::path executable;
    // Arguments after argv[0].
    std::vector<std::string> arguments;
    // Complete child environment as "NAME=value" entries; nothing is inherited implicitly.
    std::vector<std::string> environment;
    // The directory the child starts in. Without one it starts in ours, which may not exist any
    // more. A relative `executable` is looked up from this directory, not from ours.
    std::optional<std::filesystem::path> working_directory{};
};

// A child process that is terminated and reaped when the object is destroyed while still running.
// Standard input is closed; standard output and error are inherited.
class Process {
public:
    static std::expected<Process, Error> spawn(const ProcessSpec& spec);

    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;
    ~Process();

    std::int64_t id() const;

    // Waits up to `timeout` for the process to exit. Returns the exit code, or 128 plus the signal
    // number when it was killed by a signal; std::nullopt if it is still running.
    std::optional<int> wait(std::chrono::milliseconds timeout);

    void terminate();
    void kill();

private:
    struct Impl;
    explicit Process(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Starts a process that goes on after this one ends, such as another window of the editor. It has a
// session of its own, nothing waits for it, and nothing kills it; this process collects its exit
// status when it ends while this one still runs. Returns its process id. The specification is read
// as for `Process::spawn`.
std::expected<std::int64_t, Error> spawn_detached(const ProcessSpec& spec);

std::int64_t current_process_id();

// The environment of the current process as "NAME=value" entries.
std::vector<std::string> current_environment();

} // namespace anvil::platform
