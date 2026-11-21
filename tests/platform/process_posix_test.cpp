#include "anvil/platform/process.hpp"

#include <csignal>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
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
