#include "anvil/core/workspace.hpp"
#include "anvil/exthost/extension_host.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using nlohmann::json;
using std::chrono::milliseconds;
namespace fs = std::filesystem;

// Activation events through the real extension host. Each fixture extension registers a command
// named after itself when it activates, so the commands of the host tell which are active. Needs
// the pinned checkout from scripts/fetch-vscode.sh and Node.js; the tests skip when either is
// missing.
class RealActivation : public ::testing::Test {
protected:
    void SetUp() override {
        node_ = ANVIL_NODE_EXECUTABLE;
        vscode_dir_ = ANVIL_VSCODE_DIR;
        if (node_.empty() || !fs::exists(vscode_dir_ / "out" / "bootstrap-fork.js")) {
            GTEST_SKIP()
                << "needs Node.js and the transpiled VS Code checkout (scripts/fetch-vscode.sh)";
        }
        scratch_ = fs::weakly_canonical(fs::temp_directory_path()) /
                   ("anvil-activation-it-" + std::to_string(::getpid()));
        fs::remove_all(scratch_);
        fs::create_directories(scratch_ / "project");
    }
    void TearDown() override {
        host_.reset();
        std::error_code ignored;
        fs::remove_all(scratch_, ignored);
    }

    // Starts the host without waiting for the startup sequence.
    void begin(std::vector<fs::path> extensions,
               std::optional<anvil::core::Workspace> workspace = {}) {
        auto started = ExtensionHost::start(ExtensionHostOptions{
            .node_executable = node_,
            .vscode_dir = vscode_dir_,
            .extension_dirs = std::move(extensions),
            .storage_dir = scratch_ / "storage",
            .workspace = std::move(workspace),
        });
        ASSERT_TRUE(started.has_value()) << started.error().message;
        host_ = std::move(*started);
    }

    void start(std::vector<fs::path> extensions,
               std::optional<anvil::core::Workspace> workspace = {}) {
        begin(std::move(extensions), std::move(workspace));
        ASSERT_TRUE(pump_until([&] { return host_->state() != HostState::starting; }));
        ASSERT_EQ(host_->state(), HostState::ready) << host_->failure();
    }

    template <class Done> bool pump_until(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + milliseconds(30000);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            const auto pumped = host_->pump(milliseconds(20));
            if (!pumped || !*pumped) {
                break;
            }
        }
        return done();
    }

    void pump_for(milliseconds duration) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto pumped = host_->pump(milliseconds(20)); !pumped || !*pumped) {
                return;
            }
        }
    }

    bool active(const std::string& name) {
        return host_->commands().extension_commands().contains("anvil.activation." + name);
    }

    json run(const std::string& command, const json& args) {
        std::optional<CallResult> result;
        host_->commands().execute(command, args.dump(),
                                  [&](CallResult r) { result = std::move(r); });
        EXPECT_TRUE(pump_until([&] { return result.has_value(); })) << command;
        if (!result || !result->has_value() || !**result) {
            return json();
        }
        return json::parse(***result);
    }

    fs::path node_;
    fs::path vscode_dir_;
    fs::path scratch_;
    std::unique_ptr<ExtensionHost> host_;
};

TEST_F(RealActivation, AnOnFileSystemExtensionStartsWhenAnotherExtensionUsesItsScheme) {
    start({ANVIL_ACTIVATION_FS_EXTENSION, ANVIL_FILESYSTEM_EXTENSION});
    pump_for(milliseconds(300));
    ASSERT_FALSE(active("activation-fs"));

    // No provider is registered, so the operation fails, after the extension has started.
    const json result = run("anvil.fs.statUri", json::array({"memfs:/a.txt"}));
    EXPECT_TRUE(active("activation-fs"));
    EXPECT_EQ(result["error"]["code"], "Unavailable");
}

TEST_F(RealActivation, ALocalResourceDoesNotFireAFileSystemEvent) {
    start({ANVIL_ACTIVATION_FS_EXTENSION, ANVIL_FILESYSTEM_EXTENSION});

    run("anvil.fs.statUri", json::array({"file:///tmp"}));
    pump_for(milliseconds(300));
    EXPECT_FALSE(active("activation-fs"));
}

TEST_F(RealActivation, TheHostActivatesOnStartupFinishedExtensionsItself) {
    start({ANVIL_ACTIVATION_STARTUP_EXTENSION});
    EXPECT_TRUE(pump_until([&] { return active("activation-startup"); }));
}

TEST_F(RealActivation, TheHostActivatesAnExtensionForAFileNameInTheWorkspace) {
    std::ofstream(scratch_ / "project" / "marker.txt") << "x";
    const auto workspace = anvil::core::Workspace::open(scratch_ / "project");
    ASSERT_TRUE(workspace.has_value());
    start({ANVIL_ACTIVATION_CONTAINS_EXTENSION}, *workspace);
    EXPECT_TRUE(pump_until([&] { return active("activation-contains"); }));
}

TEST_F(RealActivation, AWorkspaceWithoutTheFileDoesNotActivateIt) {
    const auto workspace = anvil::core::Workspace::open(scratch_ / "project");
    ASSERT_TRUE(workspace.has_value());
    start({ANVIL_ACTIVATION_CONTAINS_EXTENSION, ANVIL_ACTIVATION_STARTUP_EXTENSION}, *workspace);
    ASSERT_TRUE(pump_until([&] { return active("activation-startup"); }))
        << "startup finished, so the check of the workspace is over";
    EXPECT_FALSE(active("activation-contains"));
}

TEST_F(RealActivation, AnEventFiredBeforeTheStartupSequenceHasRunWaitsForIt) {
    begin({ANVIL_ACTIVATION_FS_EXTENSION});
    ASSERT_EQ(host_->state(), HostState::starting);

    std::optional<CallResult> result;
    host_->fire_activation_event("onFileSystem:memfs", ActivationKind::normal,
                                 [&](CallResult r) { result = std::move(r); });
    EXPECT_FALSE(result.has_value());

    ASSERT_TRUE(pump_until([&] { return result.has_value(); }));
    EXPECT_TRUE(result->has_value()) << result->error();
    EXPECT_TRUE(active("activation-fs")) << "the reply comes when the activation has finished";
}

TEST_F(RealActivation, AnEventThatWaitsFailsWhenTheHostIsAbandoned) {
    begin({ANVIL_ACTIVATION_FS_EXTENSION});
    std::optional<CallResult> result;
    host_->fire_activation_event("onFileSystem:memfs", ActivationKind::normal,
                                 [&](CallResult r) { result = std::move(r); });

    host_->abandon("the extension host restarted");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_NE(result->error().find("restarted"), std::string::npos);
}
