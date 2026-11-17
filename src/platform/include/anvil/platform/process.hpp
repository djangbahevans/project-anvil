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

std::int64_t current_process_id();

// The environment of the current process as "NAME=value" entries.
std::vector<std::string> current_environment();

} // namespace anvil::platform
