#include "anvil/core/workspace.hpp"
#include "anvil/exthost/extension_host.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using nlohmann::json;
using std::chrono::milliseconds;
namespace fs = std::filesystem;

// `workspace.createFileSystemWatcher` through the real extension host: changes that the test makes
// from outside and through `workspace.fs` reach the watchers of an extension. Needs the pinned
// checkout from scripts/fetch-vscode.sh and Node.js; the tests skip when either is missing.
class RealFileEvents : public ::testing::Test {
protected:
    void SetUp() override {
        node_ = ANVIL_NODE_EXECUTABLE;
        vscode_dir_ = ANVIL_VSCODE_DIR;
        if (node_.empty() || !fs::exists(vscode_dir_ / "out" / "bootstrap-fork.js")) {
            GTEST_SKIP()
                << "needs Node.js and the transpiled VS Code checkout (scripts/fetch-vscode.sh)";
        }
        scratch_ = fs::weakly_canonical(fs::temp_directory_path()) /
                   ("anvil-file-events-it-" + std::to_string(::getpid()));
        fs::remove_all(scratch_);
        fs::create_directories(scratch_ / "watched");
        fs::create_directories(scratch_ / "project");
    }
    void TearDown() override {
        host_.reset();
        std::error_code ignored;
        fs::remove_all(scratch_, ignored);
    }

    void start(std::optional<anvil::core::Workspace> workspace = std::nullopt) {
        auto started = ExtensionHost::start(ExtensionHostOptions{
            .node_executable = node_,
            .vscode_dir = vscode_dir_,
            .extension_dirs = {ANVIL_WATCHER_EXTENSION},
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

    // Services the connection for `duration`.
    void pump_for(milliseconds duration) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto pumped = host_->pump(milliseconds(20)); !pumped || !*pumped) {
                return;
            }
        }
    }

    json run(const std::string& command, const json& args = json::array()) {
        std::optional<CallResult> result;
        host_->commands().execute("anvil.watch." + command, args.dump(),
                                  [&](CallResult r) { result = std::move(r); });
        EXPECT_TRUE(pump_until([&] { return result.has_value(); })) << command;
        if (!result || !result->has_value()) {
            ADD_FAILURE() << command << " failed";
            return json();
        }
        return **result ? json::parse(***result) : json();
    }

    static void write(const fs::path& path, const std::string& text) {
        std::ofstream(path, std::ios::app) << text;
    }

    // Waits until a watcher has recorded `kind` for the path.
    bool seen(int watcher, const std::string& kind, const fs::path& path) {
        const auto has = [&] {
            for (const json& event : run("events")) {
                if (event["watcher"] == watcher && event["kind"] == kind &&
                    event["path"] == path.generic_string()) {
                    return true;
                }
            }
            return false;
        };
        const auto deadline = std::chrono::steady_clock::now() + milliseconds(10000);
        while (!has() && std::chrono::steady_clock::now() < deadline) {
            pump_for(milliseconds(50));
        }
        return has();
    }

    fs::path node_;
    fs::path vscode_dir_;
    fs::path scratch_;
    std::unique_ptr<ExtensionHost> host_;
};

TEST_F(RealFileEvents, AWatcherOfAFolderSeesChangesFromOutside) {
    start();
    const int watcher = run("create", {(scratch_ / "watched").string(), "**/*.txt"}).get<int>();
    const fs::path file = scratch_ / "watched" / "a.txt";

    write(file, "one");
    ASSERT_TRUE(seen(watcher, "create", file));
    write(file, "two");
    ASSERT_TRUE(seen(watcher, "change", file));
    fs::remove(file);
    ASSERT_TRUE(seen(watcher, "delete", file));
}

TEST_F(RealFileEvents, APatternWithAGlobstarWatchesTheFoldersBelow) {
    start();
    const int deep = run("create", {(scratch_ / "watched").string(), "**/*.txt"}).get<int>();
    const int flat = run("create", {(scratch_ / "watched").string(), "*.txt"}).get<int>();

    fs::create_directories(scratch_ / "watched" / "sub");
    write(scratch_ / "watched" / "sub" / "nested.txt", "x");
    ASSERT_TRUE(seen(deep, "create", scratch_ / "watched" / "sub" / "nested.txt"));
    write(scratch_ / "watched" / "top.txt", "x");
    ASSERT_TRUE(seen(flat, "create", scratch_ / "watched" / "top.txt"));
}

TEST_F(RealFileEvents, AWatcherSeesChangesThatTheExtensionMakesThroughWorkspaceFs) {
    start();
    const int watcher = run("create", {(scratch_ / "watched").string(), "**/*"}).get<int>();
    const fs::path file = scratch_ / "watched" / "b.txt";
    const fs::path renamed = scratch_ / "watched" / "c.txt";

    run("write", {file.string(), "one"});
    ASSERT_TRUE(seen(watcher, "create", file));
    run("write", {file.string(), "two"});
    ASSERT_TRUE(seen(watcher, "change", file));
    run("rename", {file.string(), renamed.string()});
    ASSERT_TRUE(seen(watcher, "delete", file));
    ASSERT_TRUE(seen(watcher, "create", renamed));
    run("delete", {renamed.string()});
    ASSERT_TRUE(seen(watcher, "delete", renamed));
}

TEST_F(RealFileEvents, APatternOnlyReportsTheFilesItMatches) {
    start();
    const int watcher = run("create", {(scratch_ / "watched").string(), "**/*.txt"}).get<int>();

    write(scratch_ / "watched" / "skipped.md", "x");
    write(scratch_ / "watched" / "kept.txt", "x");
    ASSERT_TRUE(seen(watcher, "create", scratch_ / "watched" / "kept.txt"));
    pump_for(milliseconds(200));
    for (const json& event : run("events")) {
        EXPECT_NE(event["path"], (scratch_ / "watched" / "skipped.md").generic_string());
    }
}

TEST_F(RealFileEvents, AStringPatternSeesTheChangesOfTheWorkspaceFolder) {
    const auto workspace = anvil::core::Workspace::open(scratch_ / "project");
    ASSERT_TRUE(workspace.has_value());
    start(*workspace);
    const int watcher = run("createGlob", {"**/*.txt"}).get<int>();

    write(scratch_ / "watched" / "outside.txt", "x");
    fs::create_directories(scratch_ / "project" / "src");
    write(scratch_ / "project" / "src" / "inside.txt", "x");
    ASSERT_TRUE(seen(watcher, "create", scratch_ / "project" / "src" / "inside.txt"));
    for (const json& event : run("events")) {
        EXPECT_NE(event["path"], (scratch_ / "watched" / "outside.txt").generic_string())
            << "a string pattern reports the workspace only";
    }
}

TEST_F(RealFileEvents, ADisposedWatcherSeesNothingMore) {
    start();
    const int first = run("create", {(scratch_ / "watched").string(), "**/*"}).get<int>();
    const int second = run("create", {(scratch_ / "watched").string(), "**/*"}).get<int>();

    run("dispose", {first});
    write(scratch_ / "watched" / "a.txt", "x");
    ASSERT_TRUE(seen(second, "create", scratch_ / "watched" / "a.txt"));
    for (const json& event : run("events")) {
        EXPECT_NE(event["watcher"], first);
    }

    run("dispose", {second});
    write(scratch_ / "watched" / "b.txt", "x");
    pump_for(milliseconds(500));
    for (const json& event : run("events")) {
        EXPECT_NE(event["path"], (scratch_ / "watched" / "b.txt").generic_string());
    }
}
