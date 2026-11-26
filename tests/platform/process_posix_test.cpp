#include "anvil/platform/process.hpp"

#include <array>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <thread>
#include <unistd.h>

using anvil::platform::Process;
using anvil::platform::ProcessSpec;
using std::chrono::milliseconds;

namespace {

ProcessSpec shell(const std::string& script, std::vector<std::string> environment = {}) {
    return ProcessSpec{"/bin/sh", {"-c", script}, std::move(environment)};
}

} // namespace

TEST(Process, ReportsTheExitCode) {
    auto process = Process::spawn(shell("exit 3"));
    ASSERT_TRUE(process.has_value()) << process.error().message;
    EXPECT_EQ(process->wait(milliseconds(5000)), 3);
}

TEST(Process, ChildSeesOnlyTheGivenEnvironment) {
    auto process = Process::spawn(
        shell(R"(test "$ANVIL_TEST_VAR" = value && test -z "$HOME")", {"ANVIL_TEST_VAR=value"}));
    ASSERT_TRUE(process.has_value()) << process.error().message;
    EXPECT_EQ(process->wait(milliseconds(5000)), 0);
}

TEST(Process, WaitTimesOutWhileTheProcessRuns) {
    auto process = Process::spawn(shell("exec sleep 30"));
    ASSERT_TRUE(process.has_value());
    EXPECT_EQ(process->wait(milliseconds(50)), std::nullopt);
}

TEST(Process, KilledProcessReportsSignalExitCode) {
    auto process = Process::spawn(shell("exec sleep 30"));
    ASSERT_TRUE(process.has_value());
    process->kill();
    EXPECT_EQ(process->wait(milliseconds(5000)), 128 + SIGKILL);
}

TEST(Process, TerminateStopsTheProcess) {
    auto process = Process::spawn(shell("exec sleep 30"));
    ASSERT_TRUE(process.has_value());
    process->terminate();
    EXPECT_EQ(process->wait(milliseconds(5000)), 128 + SIGTERM);
}

TEST(Process, DestructorKillsARunningProcess) {
    std::int64_t pid = 0;
    {
        auto process = Process::spawn(shell("exec sleep 30"));
        ASSERT_TRUE(process.has_value());
        pid = process->id();
        ASSERT_EQ(::kill(static_cast<pid_t>(pid), 0), 0);
    }
    EXPECT_EQ(::kill(static_cast<pid_t>(pid), 0), -1);
}

TEST(Process, SpawnFailsForAMissingExecutable) {
    const auto process = Process::spawn(ProcessSpec{"/nonexistent/anvil-test-binary", {}, {}});
    ASSERT_FALSE(process.has_value());
    EXPECT_EQ(process.error().code, anvil::ErrorCode::io_error);
}

TEST(CurrentEnvironment, ContainsEntriesOfTheCallingProcess) {
    ::setenv("ANVIL_TEST_ENV_ENTRY", "present", 1);
    const auto environment = anvil::platform::current_environment();
    EXPECT_NE(std::find(environment.begin(), environment.end(), "ANVIL_TEST_ENV_ENTRY=present"),
              environment.end());
}

TEST(Process, RunsInTheGivenWorkingDirectory) {
    const auto directory = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                           ("anvil-process-cwd-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    const auto output = directory / "cwd.txt";
    ProcessSpec spec = shell("exec pwd -P > '" + output.string() + "'");
    spec.working_directory = directory;

    auto process = Process::spawn(spec);
    ASSERT_TRUE(process.has_value()) << process.error().message;
    EXPECT_EQ(process->wait(milliseconds(5000)), 0);
    std::string line;
    std::getline(std::ifstream(output), line);
    std::filesystem::remove_all(directory);
    EXPECT_EQ(line, directory.string());
}

TEST(Process, WithoutAWorkingDirectoryTheChildKeepsOurs) {
    const std::string ours = std::filesystem::current_path().string();
    auto process = Process::spawn(shell("test \"$(pwd -P)\" = '" + ours + "'"));
    ASSERT_TRUE(process.has_value()) << process.error().message;
    EXPECT_EQ(process->wait(milliseconds(5000)), 0);
}

TEST(Process, AWorkingDirectoryThatCannotBeEnteredIsReportedByName) {
    ProcessSpec spec = shell("exit 0");
    spec.working_directory = "/nonexistent/anvil-working-directory";
    const auto process = Process::spawn(spec);
    ASSERT_FALSE(process.has_value());
    EXPECT_NE(process.error().message.find("/nonexistent/anvil-working-directory"),
              std::string::npos)
        << process.error().message;
}

TEST(Process, TheCurrentProcessIdIsOurs) {
    EXPECT_EQ(anvil::platform::current_process_id(), static_cast<std::int64_t>(::getpid()));
}

namespace {

class SpawnDetachedTest : public ::testing::Test {
protected:
    void SetUp() override {
        scratch_ = std::filesystem::temp_directory_path() /
                   ("anvil-detached-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(scratch_);
    }
    void TearDown() override {
        std::filesystem::remove_all(scratch_);
    }

    // Waits until the file has `count` lines, and returns them.
    std::vector<std::string> lines_of(const std::filesystem::path& file, std::size_t count) {
        std::vector<std::string> lines;
        for (int i = 0; i < 1000 && lines.size() < count; ++i) {
            lines.clear();
            std::ifstream in(file);
            for (std::string line; std::getline(in, line);) {
                lines.push_back(line);
            }
            if (lines.size() < count) {
                std::this_thread::sleep_for(milliseconds(5));
            }
        }
        return lines;
    }

    std::filesystem::path scratch_;
};

} // namespace

TEST_F(SpawnDetachedTest, StartsTheProgramWithItsArgumentsInTheDirectoryItWasGiven) {
    ProcessSpec spec{"/bin/sh",
                     {"-c", "pwd > out.txt; echo \"$1\" >> out.txt", "sh", "argument"},
                     {"PATH=/usr/bin:/bin"},
                     scratch_};
    const auto pid = anvil::platform::spawn_detached(spec);
    ASSERT_TRUE(pid.has_value()) << pid.error().message;
    EXPECT_GT(*pid, 0);
    EXPECT_EQ(lines_of(scratch_ / "out.txt", 2),
              (std::vector<std::string>{scratch_.string(), "argument"}));
}

TEST_F(SpawnDetachedTest, ReportsAProgramThatCannotBeStarted) {
    const auto pid = anvil::platform::spawn_detached(ProcessSpec{scratch_ / "missing", {}, {}});
    ASSERT_FALSE(pid.has_value());
    EXPECT_NE(pid.error().message.find("cannot start"), std::string::npos);
    const auto elsewhere = anvil::platform::spawn_detached(
        ProcessSpec{"/bin/sh", {"-c", "exit 0"}, {}, scratch_ / "no-such-directory"});
    ASSERT_FALSE(elsewhere.has_value());
    EXPECT_NE(elsewhere.error().message.find("is not a directory"), std::string::npos);
}

#if defined(__linux__)
namespace {

// The process id and the session id of a process, from /proc.
std::pair<long, long> ids_of(long pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string text;
    std::getline(in, text);
    const std::size_t close = text.rfind(')');
    std::istringstream rest(text.substr(close + 2));
    std::string state;
    long parent = 0;
    long group = 0;
    long session = 0;
    rest >> state >> parent >> group >> session;
    return {parent, session};
}

} // namespace

TEST_F(SpawnDetachedTest, TheProcessHasASessionOfItsOwn) {
    const auto pid = anvil::platform::spawn_detached(
        ProcessSpec{"/bin/sh", {"-c", "exec sleep 30"}, {"PATH=/usr/bin:/bin"}});
    ASSERT_TRUE(pid.has_value());
    const auto [parent, session] = ids_of(static_cast<long>(*pid));
    EXPECT_EQ(parent, static_cast<long>(::getpid()));
    EXPECT_EQ(session, static_cast<long>(*pid));
    ::kill(static_cast<pid_t>(*pid), SIGKILL);
}

TEST_F(SpawnDetachedTest, AProcessThatEndsIsCollectedAndLeavesNoZombie) {
    const auto pid = anvil::platform::spawn_detached(ProcessSpec{"/bin/sh", {"-c", "exit 0"}, {}});
    ASSERT_TRUE(pid.has_value());
    const std::filesystem::path stat = "/proc/" + std::to_string(*pid);
    for (int i = 0; i < 1000 && std::filesystem::exists(stat); ++i) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    EXPECT_FALSE(std::filesystem::exists(stat));
}
#endif

namespace {

ProcessSpec piped(const std::string& script, bool input, bool output, bool error) {
    ProcessSpec spec = shell(script, {"PATH=/usr/bin:/bin"});
    spec.pipe_input = input;
    spec.pipe_output = output;
    spec.pipe_error = error;
    return spec;
}

std::span<const std::byte> bytes_of(const std::string& text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

struct Collected {
    std::string output;
    std::string error;
    bool output_closed = false;
    bool error_closed = false;
    std::size_t written = 0;
};

// Serves the streams as one thread does: writes `input`, closing it when it is all written, and
// reads what is piped, until the streams of `want` are closed or ten seconds are over.
Collected serve(Process& process, const std::string& input, bool want_output, bool want_error) {
    Collected result;
    bool input_open = !input.empty();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::array<std::byte, 8192> buffer{};
    const auto take = [&](std::expected<anvil::platform::ReadResult, anvil::Error> read,
                          std::string& into, bool& closed) {
        EXPECT_TRUE(read.has_value()) << read.error().message;
        if (!read) {
            closed = true;
            return;
        }
        into.append(reinterpret_cast<const char*>(buffer.data()), read->bytes);
        closed = read->closed;
    };
    while (((want_output && !result.output_closed) || (want_error && !result.error_closed)) &&
           std::chrono::steady_clock::now() < deadline) {
        if (input_open) {
            const auto written =
                process.write_input(bytes_of(input).subspan(result.written), milliseconds(0));
            EXPECT_TRUE(written.has_value()) << written.error().message;
            result.written += written.value_or(0);
            if (!written || result.written == input.size()) {
                process.close_input();
                input_open = false;
            }
        }
        EXPECT_TRUE(process.wait_for_output(milliseconds(input_open ? 1 : 100)).has_value());
        if (want_output && !result.output_closed) {
            take(process.read_output(buffer, milliseconds(0)), result.output, result.output_closed);
        }
        if (want_error && !result.error_closed) {
            take(process.read_error(buffer, milliseconds(0)), result.error, result.error_closed);
        }
    }
    return result;
}

std::size_t open_descriptors() {
    std::size_t count = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        count += ::fcntl(fd, F_GETFD) != -1 ? 1 : 0;
    }
    return count;
}

} // namespace

TEST(ProcessStreams, NothingIsPipedUnlessItIsAskedFor) {
    // The input of the child is empty: `cat` ends at once.
    auto process = Process::spawn(shell("exec cat", {"PATH=/usr/bin:/bin"}));
    ASSERT_TRUE(process.has_value()) << process.error().message;
    EXPECT_EQ(process->wait(milliseconds(5000)), 0);
    std::array<std::byte, 16> buffer{};
    EXPECT_EQ(process->write_input(bytes_of("a"), milliseconds(0)).error().code,
              anvil::ErrorCode::invalid_argument);
    EXPECT_EQ(process->read_output(buffer, milliseconds(0)).error().code,
              anvil::ErrorCode::invalid_argument);
    EXPECT_EQ(process->read_error(buffer, milliseconds(0)).error().code,
              anvil::ErrorCode::invalid_argument);
    EXPECT_EQ(process->wait_for_output(milliseconds(0)), false);
}

TEST(ProcessStreams, InputIsWrittenAndClosedAndOutputAndErrorAreReadApart) {
    auto process = Process::spawn(
        piped("read line; echo \"out: $line\"; echo problem >&2; exec cat", true, true, true));
    ASSERT_TRUE(process.has_value()) << process.error().message;
    const Collected got = serve(*process, "first\nrest of the input\n", true, true);
    EXPECT_EQ(got.output, "out: first\nrest of the input\n");
    EXPECT_EQ(got.error, "problem\n");
    EXPECT_TRUE(got.output_closed);
    EXPECT_TRUE(got.error_closed);
    EXPECT_EQ(process->wait(milliseconds(5000)), 0);
    EXPECT_EQ(process->write_input(bytes_of("a"), milliseconds(0)).error().code,
              anvil::ErrorCode::invalid_argument);
}

TEST(ProcessStreams, AReadTimesOutWhileTheChildSaysNothing) {
    auto process = Process::spawn(piped("exec sleep 30", false, true, false));
    ASSERT_TRUE(process.has_value());
    std::array<std::byte, 16> buffer{};
    const auto start = std::chrono::steady_clock::now();
    const auto read = process->read_output(buffer, milliseconds(100));
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->bytes, 0U);
    EXPECT_FALSE(read->closed);
    EXPECT_GE(std::chrono::steady_clock::now() - start, milliseconds(90));
    EXPECT_EQ(process->wait_for_output(milliseconds(10)), false);
}

TEST(ProcessStreams, WhatTheChildWroteIsReadAfterItHasExitedAndThenTheStreamIsClosed) {
    auto process = Process::spawn(piped("echo kept", false, true, false));
    ASSERT_TRUE(process.has_value());
    ASSERT_EQ(process->wait(milliseconds(5000)), 0);
    const Collected got = serve(*process, "", true, false);
    EXPECT_EQ(got.output, "kept\n");
    EXPECT_TRUE(got.output_closed);
    std::array<std::byte, 16> buffer{};
    const auto again = process->read_output(buffer, milliseconds(0));
    ASSERT_TRUE(again.has_value());
    EXPECT_TRUE(again->closed);
    // A stream that was reported closed is not waited for.
    EXPECT_EQ(process->wait_for_output(milliseconds(0)), false);
}

TEST(ProcessStreams, MoreThanAPipeHoldsIsReadFromBothStreamsWithoutEitherSideBlocking) {
    constexpr std::size_t size = 4 * 1024 * 1024;
    auto process = Process::spawn(
        piped("head -c 4194304 /dev/zero; head -c 4194304 /dev/zero >&2; head -c 100 /dev/zero",
              false, true, true));
    ASSERT_TRUE(process.has_value());
    const Collected got = serve(*process, "", true, true);
    EXPECT_EQ(got.output.size(), size + 100);
    EXPECT_EQ(got.error.size(), size);
    EXPECT_EQ(process->wait(milliseconds(5000)), 0);
}

TEST(ProcessStreams, InputLargerThanThePipesIsWrittenInFullWhileTheChildEchoesIt) {
    std::string input(4 * 1024 * 1024, 'x');
    for (std::size_t i = 0; i < input.size(); i += 4096) {
        input[i] = static_cast<char>('a' + (i / 4096) % 26);
    }
    auto process = Process::spawn(piped("exec cat", true, true, false));
    ASSERT_TRUE(process.has_value());
    const Collected got = serve(*process, input, true, false);
    EXPECT_EQ(got.written, input.size());
    EXPECT_EQ(got.output, input);
    EXPECT_EQ(process->wait(milliseconds(5000)), 0);
}

TEST(ProcessStreams, AWriteTimesOutOnceAChildThatDoesNotReadHasAFullPipe) {
    auto process = Process::spawn(piped("exec sleep 30", true, false, false));
    ASSERT_TRUE(process.has_value());
    const std::string chunk(65536, 'x');
    std::size_t total = 0;
    std::optional<std::size_t> last;
    for (int i = 0; i < 100 && last != 0U; ++i) {
        const auto written = process->write_input(bytes_of(chunk), milliseconds(20));
        ASSERT_TRUE(written.has_value()) << written.error().message;
        last = *written;
        total += *written;
    }
    EXPECT_EQ(last, 0U);
    EXPECT_GT(total, 0U);
}

TEST(ProcessStreams, WritingToAChildThatHasExitedFailsAndDoesNotEndThisProcess) {
    // The default disposition, which ends a process that writes to a pipe without a reader; a
    // test runner may have it ignored.
    const auto previous = std::signal(SIGPIPE, SIG_DFL);
    auto process = Process::spawn(piped("exit 0", true, false, false));
    ASSERT_TRUE(process.has_value());
    ASSERT_EQ(process->wait(milliseconds(5000)), 0);
    for (int i = 0; i < 3; ++i) {
        const auto written = process->write_input(bytes_of("late"), milliseconds(100));
        ASSERT_FALSE(written.has_value());
        EXPECT_EQ(written.error().code, anvil::ErrorCode::io_error);
    }
    sigset_t pending;
    sigpending(&pending);
    EXPECT_EQ(sigismember(&pending, SIGPIPE), 0);
    sigset_t blocked;
    pthread_sigmask(SIG_BLOCK, nullptr, &blocked);
    EXPECT_EQ(sigismember(&blocked, SIGPIPE), 0);
    std::signal(SIGPIPE, previous);
}

TEST(ProcessStreams, AnotherChildDoesNotKeepTheInputOfThisOneOpen) {
    auto reader = Process::spawn(piped("exec cat", true, true, false));
    ASSERT_TRUE(reader.has_value());
    auto other = Process::spawn(shell("exec sleep 30"));
    ASSERT_TRUE(other.has_value());
    reader->close_input();
    EXPECT_EQ(reader->wait(milliseconds(5000)), 0);
}

TEST(ProcessStreams, TheStreamsWorkWhenThisProcessHasItsStandardStreamsClosed) {
    std::fflush(nullptr);
    std::array<int, 3> saved{};
    for (int fd = 0; fd < 3; ++fd) {
        saved[static_cast<std::size_t>(fd)] = ::fcntl(fd, F_DUPFD_CLOEXEC, 100);
        ::close(fd);
    }
    Collected got;
    std::optional<int> status;
    {
        auto process = Process::spawn(piped("cat; echo problem >&2", true, true, true));
        if (process) {
            got = serve(*process, "through\n", true, true);
            status = process->wait(milliseconds(5000));
        }
    }
    for (int fd = 0; fd < 3; ++fd) {
        ::dup2(saved[static_cast<std::size_t>(fd)], fd);
        ::close(saved[static_cast<std::size_t>(fd)]);
    }
    EXPECT_EQ(got.output, "through\n");
    EXPECT_EQ(got.error, "problem\n");
    EXPECT_EQ(status, 0);
}

TEST(ProcessStreams, NoDescriptorStaysBehindAChildOrAFailedStart) {
    const std::size_t before = open_descriptors();
    {
        auto process = Process::spawn(piped("exit 0", true, true, true));
        ASSERT_TRUE(process.has_value());
        // Ours only: the ends of the child are closed once it has them.
        EXPECT_EQ(open_descriptors(), before + 3);
    }
    EXPECT_EQ(open_descriptors(), before);
    ProcessSpec missing{"/nonexistent/anvil-test-binary", {}, {}};
    missing.pipe_input = missing.pipe_output = missing.pipe_error = true;
    EXPECT_FALSE(Process::spawn(missing).has_value());
    EXPECT_EQ(open_descriptors(), before);
}

TEST(ProcessStreams, AProcessThatOutlivesThisOneCannotHavePipes) {
    for (int stream = 0; stream < 3; ++stream) {
        const auto pid =
            anvil::platform::spawn_detached(piped("exit 0", stream == 0, stream == 1, stream == 2));
        ASSERT_FALSE(pid.has_value()) << stream;
        EXPECT_EQ(pid.error().code, anvil::ErrorCode::invalid_argument);
    }
}
