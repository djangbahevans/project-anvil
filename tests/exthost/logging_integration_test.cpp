#include "anvil/exthost/extension_host.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using nlohmann::json;
using std::chrono::milliseconds;
namespace core = anvil::core;
using core::LogLevel;

// What extensions log and the errors they raise, as seen through the real extension host. Needs
// the pinned checkout from scripts/fetch-vscode.sh and Node.js; the tests skip when either is
// missing.
class RealLogging : public ::testing::Test {
protected:
    void SetUp() override {
        const std::filesystem::path vscode_dir = ANVIL_VSCODE_DIR;
        const std::filesystem::path node = ANVIL_NODE_EXECUTABLE;
        if (node.empty() || !std::filesystem::exists(vscode_dir / "out" / "bootstrap-fork.js")) {
            GTEST_SKIP()
                << "needs Node.js and the transpiled VS Code checkout (scripts/fetch-vscode.sh)";
        }
        scratch_ =
            std::filesystem::temp_directory_path() / ("anvil-log-it-" + std::to_string(::getpid()));
        std::filesystem::create_directories(scratch_);
        logger_ = std::make_shared<core::RecordingLogger>();

        auto started = ExtensionHost::start(ExtensionHostOptions{
            .node_executable = node,
            .vscode_dir = vscode_dir,
            .extension_dirs = {ANVIL_LOGGING_EXTENSION, ANVIL_FAILING_EXTENSION},
            .storage_dir = scratch_,
            .logger = logger_,
        });
        ASSERT_TRUE(started.has_value()) << started.error().message;
        host_ = std::move(*started);
        ASSERT_TRUE(pump_until([&] { return host_->state() != HostState::starting; }));
        ASSERT_EQ(host_->state(), HostState::ready) << host_->failure();
    }

    void TearDown() override {
        host_.reset();
        std::filesystem::remove_all(scratch_);
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

    void run(const std::string& command) {
        std::optional<CallResult> result;
        host_->commands().execute(command, "[]", [&](CallResult r) { result = std::move(r); });
        ASSERT_TRUE(pump_until([&] { return result.has_value(); }));
        ASSERT_TRUE(result->has_value()) << result->error();
    }

    bool eventually_logged(LogLevel level, std::string_view text, std::string_view source = {}) {
        return pump_until([&] { return logger_->contains(level, text, source); });
    }

    std::filesystem::path scratch_;
    std::shared_ptr<core::RecordingLogger> logger_;
    std::unique_ptr<ExtensionHost> host_;
};

TEST_F(RealLogging, ConsoleOutputOfExtensionsIsLogged) {
    run("anvil.log.console");
    EXPECT_TRUE(eventually_logged(LogLevel::info, R"(hello {"a":1} 42)"));
    EXPECT_TRUE(eventually_logged(LogLevel::warning, "careful now"));
    EXPECT_TRUE(eventually_logged(LogLevel::error, "bad thing happened"));
}

TEST_F(RealLogging, ActivationFailuresAreLoggedAgainstTheExtension) {
    EXPECT_TRUE(eventually_logged(LogLevel::error, "boom on activate", "anvil-fixtures.failing"));
    EXPECT_TRUE(eventually_logged(LogLevel::error, "activation failed", "anvil-fixtures.failing"));
}

TEST_F(RealLogging, ExtensionsThatActivateAreNotReportedAsFailures) {
    EXPECT_FALSE(logger_->contains(LogLevel::error, "activation failed", "anvil-fixtures.logging"));
}

TEST_F(RealLogging, LogOutputChannelsAreAttributedToTheirName) {
    run("anvil.log.channel");
    EXPECT_TRUE(eventually_logged(LogLevel::info, "from the log channel", "Anvil Log Channel"));
    EXPECT_TRUE(
        eventually_logged(LogLevel::warning, "warned by the log channel", "Anvil Log Channel"));
}

TEST_F(RealLogging, PlainOutputChannelsAreLogged) {
    run("anvil.log.plainChannel");

    EXPECT_TRUE(eventually_logged(LogLevel::info, "a plain line", "Anvil Plain Channel"));
}

TEST_F(RealLogging, UncaughtErrorsInExtensionsAreLogged) {
    run("anvil.log.unhandled");
    EXPECT_TRUE(eventually_logged(LogLevel::error, "async boom"));
}

TEST_F(RealLogging, StartupLogsNoHostErrorsWhenTheNativeModulesAreBuilt) {
    const std::filesystem::path spdlog = std::filesystem::path(ANVIL_VSCODE_DIR) /
                                         "node_modules/@vscode/spdlog/build/Release/spdlog.node";
    if (!std::filesystem::exists(spdlog)) {
        GTEST_SKIP() << "the native modules are not built (scripts/fetch-vscode.sh)";
    }
    pump_until([] { return false; }, milliseconds(1500)); // let any late startup messages arrive
    for (const auto& entry : logger_->entries()) {
        EXPECT_FALSE(entry.source == "extension host" && entry.level == LogLevel::error)
            << entry.message;
    }
}
