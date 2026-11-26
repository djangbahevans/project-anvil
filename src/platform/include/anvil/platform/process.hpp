#pragma once

#include "anvil/core/error.hpp"
#include "anvil/platform/read_result.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
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
    // Which standard streams are pipes to this process. Without a pipe the input is empty, and
    // the output and the error are ours.
    bool pipe_input = false;
    bool pipe_output = false;
    bool pipe_error = false;
};

// A child process that is terminated and reaped when the object is destroyed while still running.
//
// The streams that the specification pipes are written and read here, each with a timeout, so that
// one thread can serve all of them: a child that is not read stops once a pipe is full, and then
// takes no more input either. The input may be written by one thread while another reads the output
// and the error.
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

    // Writes what the child's input takes of `data`, waiting up to `timeout` for room. The number
    // of bytes written, zero on a timeout. Fails once the child has closed its input, as it does
    // when it exits, and when the input is not a pipe or has been closed.
    std::expected<std::size_t, Error> write_input(std::span<const std::byte> data,
                                                  std::chrono::milliseconds timeout);
    // Ends the child's input.
    void close_input();

    // Waits up to `timeout` for data of the child's output, or of its error. A result with zero
    // bytes and `closed` false is a timeout. Fails when the stream is not a pipe.
    std::expected<ReadResult, Error> read_output(std::span<std::byte> buffer,
                                                 std::chrono::milliseconds timeout);
    std::expected<ReadResult, Error> read_error(std::span<std::byte> buffer,
                                                std::chrono::milliseconds timeout);
    // Waits up to `timeout` until a piped stream that a read has not reported closed has data to
    // read, or has ended. False on a timeout, and at once when there is no such stream.
    std::expected<bool, Error> wait_for_output(std::chrono::milliseconds timeout);

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
// as for `Process::spawn`, except that a pipe, which nothing would serve, is refused.
std::expected<std::int64_t, Error> spawn_detached(const ProcessSpec& spec);

std::int64_t current_process_id();

// The environment of the current process as "NAME=value" entries.
std::vector<std::string> current_environment();

} // namespace anvil::platform
