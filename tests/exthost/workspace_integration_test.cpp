#include "anvil/core/workspace.hpp"
#include "anvil/exthost/extension_host.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using nlohmann::json;
using std::chrono::milliseconds;
namespace fs = std::filesystem;

// What extensions see of the workspace through the real extension host. Needs the pinned checkout
// from scripts/fetch-vscode.sh and Node.js; the tests skip when either is missing.
class RealWorkspace : public ::testing::Test {
protected:
    void SetUp() override {
        node_ = ANVIL_NODE_EXECUTABLE;
        vscode_dir_ = ANVIL_VSCODE_DIR;
        if (node_.empty() || !fs::exists(vscode_dir_ / "out" / "bootstrap-fork.js")) {
            GTEST_SKIP()
                << "needs Node.js and the transpiled VS Code checkout (scripts/fetch-vscode.sh)";
        }
        scratch_ = fs::weakly_canonical(fs::temp_directory_path()) /
                   ("anvil-workspace-it-" + std::to_string(::getpid()));
        fs::remove_all(scratch_);
        fs::create_directories(scratch_ / "project" / "src");
    }

    void TearDown() override {
        host_.reset();
        std::error_code ignored;
        fs::remove_all(scratch_, ignored);
    }

    void start(std::optional<anvil::core::Workspace> workspace) {
        auto started = ExtensionHost::start(ExtensionHostOptions{
            .node_executable = node_,
            .vscode_dir = vscode_dir_,
            .extension_dirs = {ANVIL_WORKSPACE_EXTENSION},
            .storage_dir = scratch_ / "storage",
            .workspace = std::move(workspace),
        });
        ASSERT_TRUE(started.has_value()) << started.error().message;
        host_ = std::move(*started);
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

    json report(const fs::path& resource) {
        std::optional<CallResult> result;
        host_->commands().execute("anvil.workspace.report", json::array({resource.string()}).dump(),
                                  [&](CallResult r) { result = std::move(r); });
        EXPECT_TRUE(pump_until([&] { return result.has_value(); }));
        if (!result || !result->has_value() || !**result) {
            ADD_FAILURE() << "the command gave no result";
            return json::object();
        }
        return json::parse(***result);
    }

    fs::path node_;
    fs::path vscode_dir_;
    fs::path scratch_;
    std::unique_ptr<ExtensionHost> host_;
};

TEST_F(RealWorkspace, ExtensionsSeeTheFolderAsTheWorkspaceFolder) {
    const auto workspace = anvil::core::Workspace::open(scratch_ / "project");
    ASSERT_TRUE(workspace.has_value());
    start(*workspace);

    const json seen = report(scratch_ / "project" / "src" / "main.cpp");
    EXPECT_EQ(seen["name"], "project");
    ASSERT_EQ(seen["folders"].size(), 1u);
    EXPECT_EQ(seen["folders"][0]["path"], (scratch_ / "project").generic_string());
    EXPECT_EQ(seen["folders"][0]["name"], "project");
    EXPECT_EQ(seen["folders"][0]["index"], 0);
    EXPECT_EQ(seen["folderOfResource"], (scratch_ / "project").generic_string());
    EXPECT_EQ(seen["relativePath"], "src/main.cpp");
    EXPECT_EQ(seen["workspaceFile"], nullptr);
    EXPECT_EQ(seen["trusted"], true);
}

TEST_F(RealWorkspace, AResourceOutsideTheFolderHasNoWorkspaceFolder) {
    const auto workspace = anvil::core::Workspace::open(scratch_ / "project");
    ASSERT_TRUE(workspace.has_value());
    start(*workspace);

    const json seen = report(scratch_ / "elsewhere.txt");
    EXPECT_EQ(seen["folderOfResource"], nullptr);
    EXPECT_EQ(seen["relativePath"], (scratch_ / "elsewhere.txt").generic_string());
}

TEST_F(RealWorkspace, WorkspaceStorageIsKeptUnderTheWorkspaceId) {
    const auto workspace = anvil::core::Workspace::open(scratch_ / "project");
    ASSERT_TRUE(workspace.has_value());
    start(*workspace);

    const json seen = report(scratch_ / "project");
    ASSERT_TRUE(seen["storage"].is_string());
    const fs::path storage = seen["storage"].get<std::string>();
    EXPECT_EQ(storage.parent_path(), scratch_ / "storage" / "workspaceStorage" / workspace->id)
        << storage;
}

TEST_F(RealWorkspace, WithoutAFolderExtensionsSeeNoWorkspace) {
    start(std::nullopt);

    const json seen = report(scratch_ / "project" / "a.txt");
    EXPECT_EQ(seen["name"], nullptr);
    EXPECT_TRUE(seen["folders"].empty());
    EXPECT_EQ(seen["folderOfResource"], nullptr);
    EXPECT_EQ(seen["storage"], nullptr);
}
