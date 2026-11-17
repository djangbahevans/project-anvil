#include "anvil/exthost/extension_host.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using nlohmann::json;
using std::chrono::milliseconds;

// Runs the unmodified VS Code extension host against the editor-side services. Needs the pinned
// checkout from scripts/fetch-vscode.sh and Node.js; the tests skip when either is missing.
class RealExtensionHost : public ::testing::Test {
protected:
    void SetUp() override {
        const std::filesystem::path vscode_dir = ANVIL_VSCODE_DIR;
        const std::filesystem::path node = ANVIL_NODE_EXECUTABLE;
        if (node.empty() || !std::filesystem::exists(vscode_dir / "out" / "bootstrap-fork.js")) {
            GTEST_SKIP()
                << "needs Node.js and the transpiled VS Code checkout (scripts/fetch-vscode.sh)";
        }

        storage_ = std::filesystem::temp_directory_path() /
                   ("anvil-host-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(storage_);

        auto started = ExtensionHost::start(ExtensionHostOptions{
            .node_executable = node,
            .vscode_dir = vscode_dir,
            .extension_dirs = {ANVIL_FIXTURE_EXTENSION},
            .storage_dir = storage_,
        });
        ASSERT_TRUE(started.has_value()) << started.error().message;
        host_ = std::move(*started);

        host_->commands().add_builtin("anvil.test.double", [](const json& args) -> CallResult {
            return std::to_string(args.at(0).get<int>() * 2);
        });
        ASSERT_TRUE(pump_until([&] { return host_->state() != HostState::starting; }));
        ASSERT_EQ(host_->state(), HostState::ready) << host_->failure();
    }

    void TearDown() override {
        host_.reset();
        std::filesystem::remove_all(storage_);
    }

    template <class Done> bool pump_until(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + milliseconds(30000);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            const auto pumped = host_->pump(milliseconds(50));
            if (!pumped) {
                ADD_FAILURE() << pumped.error().message;
                return false;
            }
            if (!*pumped) {
                break;
            }
        }
        return done();
    }

    CallResult run_command(const std::string& id, const std::string& args_json) {
        std::optional<CallResult> result;
        host_->commands().execute(id, args_json, [&](CallResult r) { result = std::move(r); });
        EXPECT_TRUE(pump_until([&] { return result.has_value(); }))
            << "command did not complete: " << id;
        return result.value_or(std::unexpected("timed out"));
    }

    std::filesystem::path storage_;
    std::unique_ptr<ExtensionHost> host_;
};

TEST_F(RealExtensionHost, ActivatesExtensionsAndTracksTheirCommands) {
    const auto& commands = host_->commands().extension_commands();
    EXPECT_TRUE(commands.contains("anvil.hello"));
    EXPECT_TRUE(commands.contains("anvil.callBuiltin"));
}

TEST_F(RealExtensionHost, RunsAnExtensionCommand) {
    const auto result = run_command("anvil.hello", R"(["anvil"])");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(**result, R"("hello, anvil")");
}

TEST_F(RealExtensionHost, ExtensionCommandsCanRunBuiltinCommands) {
    const auto result = run_command("anvil.callBuiltin", "[21]");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(**result, R"("doubled: 42")");
}

TEST_F(RealExtensionHost, ExtensionSeesAnErrorWhenItRunsAMissingCommand) {
    const auto result = run_command("anvil.callMissing", "[]");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("command 'anvil.test.missing' not found"), std::string::npos)
        << result.error();
}

TEST_F(RealExtensionHost, ExecutingAnUnknownCommandFails) {
    const auto result = run_command("no.such.command", "[]");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("not found"), std::string::npos);
}

TEST_F(RealExtensionHost, ReportsRequestsNoServiceHandled) {
    // Informational: lists the services still to implement for this extension.
    for (const auto& name : host_->unhandled_requests()) {
        RecordProperty("unhandled", name);
        std::cout << "unhandled: " << name << "\n";
    }
    SUCCEED();
}
