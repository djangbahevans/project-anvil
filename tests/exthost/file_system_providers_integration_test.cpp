#include "anvil/exthost/extension_host.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil;
using namespace anvil::exthost;
using nlohmann::json;
using std::chrono::milliseconds;
namespace fs = std::filesystem;

namespace {

core::Uri memfs(const std::string& path, const std::string& scheme = "memfs") {
    return core::Uri{scheme, "", path, "", ""};
}

} // namespace

// File system providers of extensions through the real extension host: the fixture provides an
// in-memory file system for `memfs` and a read-only one for `memro`, and the test plays the editor
// that reads and writes them, and an extension that uses `workspace.fs` on them. Needs the pinned
// checkout from scripts/fetch-vscode.sh and Node.js; the tests skip when either is missing.
class RealProviders : public ::testing::Test {
protected:
    void SetUp() override {
        const fs::path vscode_dir = ANVIL_VSCODE_DIR;
        const fs::path node = ANVIL_NODE_EXECUTABLE;
        if (node.empty() || !fs::exists(vscode_dir / "out" / "bootstrap-fork.js")) {
            GTEST_SKIP()
                << "needs Node.js and the transpiled VS Code checkout (scripts/fetch-vscode.sh)";
        }
        scratch_ = fs::weakly_canonical(fs::temp_directory_path()) /
                   ("anvil-providers-it-" + std::to_string(::getpid()));
        fs::remove_all(scratch_);
        auto started = ExtensionHost::start(ExtensionHostOptions{
            .node_executable = node,
            .vscode_dir = vscode_dir,
            .extension_dirs = {ANVIL_MEMFS_EXTENSION},
            .storage_dir = scratch_ / "storage",
        });
        ASSERT_TRUE(started.has_value()) << started.error().message;
        host_ = std::move(*started);
        ASSERT_TRUE(pump_until([&] { return host_->state() != HostState::starting; }));
        ASSERT_EQ(host_->state(), HostState::ready) << host_->failure();

        // The provider registers when its extension activates, which its scheme causes.
        std::optional<CallResult> activated;
        host_->fire_activation_event("onFileSystem:memfs", ActivationKind::normal,
                                     [&](CallResult r) { activated = std::move(r); });
        ASSERT_TRUE(pump_until([&] { return activated.has_value(); }));
        ASSERT_TRUE(
            pump_until([&] { return providers().find("memfs") && providers().find("memro"); }));
    }
    void TearDown() override {
        host_.reset();
        std::error_code ignored;
        fs::remove_all(scratch_, ignored);
    }

    FileSystemProviders& providers() {
        return host_->file_system_providers();
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

    // Runs an operation of the providers and waits for its result.
    template <class T, class Operation> std::expected<T, ProviderError> sync(Operation operation) {
        std::optional<std::expected<T, ProviderError>> result;
        operation([&](std::expected<T, ProviderError> r) { result = std::move(r); });
        EXPECT_TRUE(pump_until([&] { return result.has_value(); }));
        return result ? std::move(*result) : std::unexpected(ProviderError{"Timeout", "no result"});
    }
    std::string error_of(const std::expected<std::monostate, ProviderError>& result) {
        return result ? "" : result.error().name;
    }

    // Runs a command of the fixture, which uses `workspace.fs`, and returns {ok} or {error}.
    json run(const std::string& command, const json& args = json::array()) {
        std::optional<CallResult> result;
        host_->commands().execute("anvil.memfs." + command, args.dump(),
                                  [&](CallResult r) { result = std::move(r); });
        EXPECT_TRUE(pump_until([&] { return result.has_value(); })) << command;
        if (!result || !result->has_value() || !**result) {
            ADD_FAILURE() << command << " gave no result";
            return json::object();
        }
        return json::parse(***result);
    }

    fs::path scratch_;
    std::unique_ptr<ExtensionHost> host_;
};

TEST_F(RealProviders, TheEditorReadsAndWritesAFileOfAProvider) {
    const Bytes binary = to_bytes(std::string("one\0two", 7));
    const auto written = sync<std::monostate>(
        [&](auto done) { providers().write_file(memfs("/a.txt"), binary, std::move(done)); });
    ASSERT_TRUE(written.has_value()) << written.error().name << ": " << written.error().message;

    const auto content =
        sync<Bytes>([&](auto done) { providers().read_file(memfs("/a.txt"), std::move(done)); });
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(*content, binary);

    const auto stat =
        sync<ProviderStat>([&](auto done) { providers().stat(memfs("/a.txt"), std::move(done)); });
    ASSERT_TRUE(stat.has_value());
    EXPECT_EQ(stat->type, 1);
    EXPECT_EQ(stat->size, 7);
}

TEST_F(RealProviders, TheEditorListsCreatesRenamesCopiesAndDeletes) {
    ASSERT_TRUE(sync<std::monostate>([&](auto d) {
                    providers().create_directory(memfs("/dir"), std::move(d));
                }).has_value());
    ASSERT_TRUE(sync<std::monostate>([&](auto d) {
                    providers().write_file(memfs("/dir/a"), to_bytes("a"), std::move(d));
                }).has_value());
    ASSERT_TRUE(sync<std::monostate>([&](auto d) {
                    providers().rename(memfs("/dir/a"), memfs("/dir/b"), false, std::move(d));
                }).has_value());
    ASSERT_TRUE(sync<std::monostate>([&](auto d) {
                    providers().copy(memfs("/dir/b"), memfs("/dir/c"), false, std::move(d));
                }).has_value());

    const auto listing = sync<std::vector<std::pair<std::string, int>>>(
        [&](auto d) { providers().read_directory(memfs("/dir"), std::move(d)); });
    ASSERT_TRUE(listing.has_value());
    EXPECT_EQ(*listing, (std::vector<std::pair<std::string, int>>{{"b", 1}, {"c", 1}}));

    ASSERT_TRUE(sync<std::monostate>([&](auto d) {
                    providers().remove(memfs("/dir/b"), false, std::move(d));
                }).has_value());
    EXPECT_EQ(sync<Bytes>([&](auto d) { providers().read_file(memfs("/dir/b"), std::move(d)); })
                  .error()
                  .name,
              "EntryNotFound");
}

TEST_F(RealProviders, TheErrorsOfTheProviderKeepTheirCodes) {
    EXPECT_EQ(sync<ProviderStat>([&](auto d) { providers().stat(memfs("/missing"), std::move(d)); })
                  .error()
                  .name,
              "EntryNotFound");

    ASSERT_TRUE(sync<std::monostate>([&](auto d) {
                    providers().create_directory(memfs("/dir"), std::move(d));
                }).has_value());
    EXPECT_EQ(error_of(sync<std::monostate>(
                  [&](auto d) { providers().create_directory(memfs("/dir"), std::move(d)); })),
              "EntryExists");
    EXPECT_EQ(sync<Bytes>([&](auto d) { providers().read_file(memfs("/dir"), std::move(d)); })
                  .error()
                  .name,
              "EntryIsADirectory");
    const auto missing_listing = sync<std::vector<std::pair<std::string, int>>>(
        [&](auto d) { providers().read_directory(memfs("/missing"), std::move(d)); });
    ASSERT_FALSE(missing_listing.has_value());
    EXPECT_EQ(missing_listing.error().name, "EntryNotFound");
}

TEST_F(RealProviders, AReadOnlyProviderCanBeReadButNotChanged) {
    const auto content = sync<Bytes>(
        [&](auto d) { providers().read_file(memfs("/fixed.txt", "memro"), std::move(d)); });
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(*content, to_bytes("read only"));

    EXPECT_EQ(error_of(sync<std::monostate>([&](auto d) {
                  providers().write_file(memfs("/x", "memro"), to_bytes("x"), std::move(d));
              })),
              "NoPermissions");
    EXPECT_EQ(error_of(sync<std::monostate>([&](auto d) {
                  providers().remove(memfs("/fixed.txt", "memro"), false, std::move(d));
              })),
              "NoPermissions");
}

TEST_F(RealProviders, AnExtensionRenamesAndCopiesThroughTheEditor) {
    // The write and the read use the provider directly; rename and copy have no shortcut and
    // reach the provider through the editor.
    ASSERT_TRUE(run("write", {"memfs:/a.txt", "content"}).contains("ok"));
    EXPECT_EQ(run("read", {"memfs:/a.txt"})["ok"], "content");

    const json renamed = run("rename", {"memfs:/a.txt", "memfs:/b.txt"});
    EXPECT_TRUE(renamed.contains("ok")) << renamed.dump();
    EXPECT_EQ(sync<Bytes>([&](auto d) {
                  providers().read_file(memfs("/b.txt"), std::move(d));
              }).value_or(Bytes{}),
              to_bytes("content"));
    EXPECT_EQ(sync<Bytes>([&](auto d) { providers().read_file(memfs("/a.txt"), std::move(d)); })
                  .error()
                  .name,
              "EntryNotFound");

    const json copied = run("copy", {"memfs:/b.txt", "memfs:/c.txt"});
    EXPECT_TRUE(copied.contains("ok")) << copied.dump();
    EXPECT_EQ(run("read", {"memfs:/c.txt"})["ok"], "content");
}

TEST_F(RealProviders, AFailureOfTheProviderReachesTheExtensionAsAFileSystemError) {
    const json missing = run("rename", {"memfs:/nothing", "memfs:/other"});
    ASSERT_TRUE(missing.contains("error")) << missing.dump();
    EXPECT_EQ(missing["error"]["code"], "FileNotFound");

    const json refused = run("rename", {"memro:/fixed.txt", "memro:/moved.txt"});
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(refused["error"]["code"], "NoPermissions");
}

TEST_F(RealProviders, AWatcherOnAProvidedFolderSeesTheChangesOfTheProvider) {
    ASSERT_TRUE(sync<std::monostate>([&](auto d) {
                    providers().create_directory(memfs("/dir"), std::move(d));
                }).has_value());
    const int watcher = run("watch", {"memfs:/dir"})["ok"].get<int>();
    ASSERT_TRUE(pump_until([&] { return !run("watches", {"memfs"})["ok"].empty(); }))
        << "the provider is asked to watch";
    EXPECT_EQ(run("watches", {"memfs"})["ok"][0]["path"], "/dir");
    EXPECT_EQ(run("watches", {"memfs"})["ok"][0]["recursive"], true);

    const auto seen = [&](const std::string& kind, const std::string& uri) {
        return pump_until([&] {
            const json events = run("events");
            for (const json& event : events["ok"]) {
                if (event["watcher"] == watcher && event["kind"] == kind && event["uri"] == uri) {
                    return true;
                }
            }
            return false;
        });
    };
    ASSERT_TRUE(sync<std::monostate>([&](auto d) {
                    providers().write_file(memfs("/dir/w.txt"), to_bytes("x"), std::move(d));
                }).has_value());
    EXPECT_TRUE(seen("create", "memfs:/dir/w.txt"));
    ASSERT_TRUE(sync<std::monostate>([&](auto d) {
                    providers().write_file(memfs("/dir/w.txt"), to_bytes("y"), std::move(d));
                }).has_value());
    EXPECT_TRUE(seen("change", "memfs:/dir/w.txt"));
    ASSERT_TRUE(sync<std::monostate>([&](auto d) {
                    providers().remove(memfs("/dir/w.txt"), false, std::move(d));
                }).has_value());
    EXPECT_TRUE(seen("delete", "memfs:/dir/w.txt"));
}
