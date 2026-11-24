#include "anvil/platform/process.hpp"

#include <csignal>
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
