#include "anvil/exthost/extension_host.hpp"
#include "anvil/exthost/session_logs.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <regex>
#include <unistd.h>

using namespace anvil::exthost;
using std::chrono::milliseconds;
namespace core = anvil::core;
namespace fs = std::filesystem;

TEST(ExtensionHostLaunch, AStorageDirectoryIsRequired) {
    const auto started = ExtensionHost::start(ExtensionHostOptions{});
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code, anvil::ErrorCode::invalid_argument);
    EXPECT_NE(started.error().message.find("storage directory"), std::string::npos);
}

// How the editor launches the real extension host: where it runs, and where it writes its logs.
// Needs the pinned checkout from scripts/fetch-vscode.sh and Node.js; the tests skip when either is
// missing.
class RealLaunch : public ::testing::Test {
protected:
    void SetUp() override {
        vscode_dir_ = ANVIL_VSCODE_DIR;
        node_ = ANVIL_NODE_EXECUTABLE;
        if (node_.empty() || !fs::exists(vscode_dir_ / "out" / "bootstrap-fork.js")) {
            GTEST_SKIP()
                << "needs Node.js and the transpiled VS Code checkout (scripts/fetch-vscode.sh)";
        }
        scratch_ = fs::temp_directory_path() / ("anvil-launch-it-" + std::to_string(::getpid()));
        fs::remove_all(scratch_);
        fs::create_directories(scratch_);
        storage_ = scratch_ / "storage";
    }

    void TearDown() override {
        host_.reset();
        std::error_code ignored;
        if (!saved_working_directory_.empty()) {
            fs::current_path(saved_working_directory_, ignored);
        }
        fs::remove_all(scratch_, ignored);
    }

    // Starts a host that shares `storage_` with the ones before it, and waits until it is ready.
    void start_host() {
        host_.reset();
        logger_ = std::make_shared<core::RecordingLogger>();
        auto started = ExtensionHost::start(ExtensionHostOptions{
            .node_executable = node_,
            .vscode_dir = vscode_dir_,
            .extension_dirs = {ANVIL_FIXTURE_EXTENSION},
            .storage_dir = storage_,
            .logger = logger_,
        });
        ASSERT_TRUE(started.has_value()) << started.error().message;
        host_ = std::move(*started);
        ASSERT_TRUE(pump_until([&] { return host_->state() != HostState::starting; }));
        ASSERT_EQ(host_->state(), HostState::ready) << host_->failure();
    }

    template <class Done> bool pump_until(Done done, milliseconds limit = milliseconds(30000)) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            const auto pumped = host_->pump(milliseconds(20));
            if (!pumped || !*pumped) {
                break;
            }
        }
        return done();
    }

    // The process id in the host's own "Extension host with pid N started" log line.
    std::optional<std::int64_t> host_pid() {
        static const std::regex line(R"(Extension host with pid (\d+) started)");
        std::optional<std::int64_t> pid;
        pump_until([&] {
            for (const auto& entry : logger_->entries()) {
                std::smatch match;
                if (std::regex_search(entry.message, match, line)) {
                    pid = std::stoll(match[1]);
                }
            }
            return pid.has_value();
        });
        return pid;
    }

    std::vector<std::string> session_directories() const {
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(storage_ / "logs")) {
            if (entry.is_directory() && is_session_logs_name(entry.path().filename().string())) {
                names.push_back(entry.path().filename().string());
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    fs::path vscode_dir_;
    fs::path node_;
    fs::path scratch_;
    fs::path storage_;
    fs::path saved_working_directory_;
    std::shared_ptr<core::RecordingLogger> logger_;
    std::unique_ptr<ExtensionHost> host_;
};

TEST_F(RealLaunch, TheHostRunsInItsStorageDirectory) {
#ifndef __linux__
    GTEST_SKIP() << "reads /proc";
#endif
    ASSERT_NO_FATAL_FAILURE(start_host());
    const auto pid = host_pid();
    ASSERT_TRUE(pid.has_value());
    EXPECT_EQ(fs::read_symlink("/proc/" + std::to_string(*pid) + "/cwd"), fs::canonical(storage_));
}

// A host that inherited a deleted working directory keeps running, but prints an error at startup
// and its extensions cannot resolve relative paths (process.cwd() throws).
TEST_F(RealLaunch, TheHostGetsADirectoryThatExistsWhenTheEditorsWasDeleted) {
#ifndef __linux__
    GTEST_SKIP() << "reads /proc";
#endif
    saved_working_directory_ = fs::current_path();
    const fs::path doomed = scratch_ / "doomed";
    fs::create_directories(doomed);
    fs::current_path(doomed);
    fs::remove(doomed);

    ASSERT_NO_FATAL_FAILURE(start_host());
    EXPECT_EQ(host_->state(), HostState::ready);
    const auto pid = host_pid();
    ASSERT_TRUE(pid.has_value());
    // A deleted directory reads as "<path> (deleted)".
    EXPECT_EQ(fs::read_symlink("/proc/" + std::to_string(*pid) + "/cwd"), fs::canonical(storage_));
}

TEST_F(RealLaunch, EachSessionLogsInADirectoryOfItsOwn) {
    ASSERT_NO_FATAL_FAILURE(start_host());
    const auto first_pid = host_pid();
    ASSERT_TRUE(first_pid.has_value());
    ASSERT_EQ(session_directories().size(), 1u);

    ASSERT_NO_FATAL_FAILURE(start_host());
    const auto second_pid = host_pid();
    ASSERT_TRUE(second_pid.has_value());
    ASSERT_NE(*first_pid, *second_pid);

    // The second session shows its own start and none of the first session's lines.
    const std::string first_line = "Extension host with pid " + std::to_string(*first_pid);
    for (const auto& entry : logger_->entries()) {
        EXPECT_EQ(entry.message.find(first_line), std::string::npos) << entry.message;
    }
    const auto directories = session_directories();
    ASSERT_EQ(directories.size(), 2u);
    EXPECT_TRUE(fs::exists(storage_ / "logs" / directories.back() / "exthost.log"));
}

namespace {

// The name of a session that started on January `day` of 2020, long before any real one.
std::string old_session(unsigned day) {
    return session_logs_name(std::chrono::sys_days{std::chrono::year{2020} / std::chrono::month{1} /
                                                   std::chrono::day{day}},
                             1);
}

} // namespace

TEST_F(RealLaunch, OldSessionDirectoriesArePrunedWhenAHostStarts) {
    for (unsigned day = 1; day <= 12; ++day) {
        fs::create_directories(storage_ / "logs" / old_session(day));
    }
    ASSERT_NO_FATAL_FAILURE(start_host());
    const auto directories = session_directories();
    EXPECT_EQ(directories.size(), kept_log_sessions);
    // The newest nine of the old ones are kept along with the current session, which sorts last.
    EXPECT_EQ(directories.front(), old_session(4));
    EXPECT_EQ(directories[8], old_session(12));
}
