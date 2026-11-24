#include "anvil/exthost/file_event_service.hpp"
#include "anvil/exthost/vscode_types.hpp"
#include "fake_host.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

using namespace anvil::exthost;
using anvil::exthost::test::FakeHost;
using nlohmann::json;
namespace core = anvil::core;
namespace fs = std::filesystem;
using std::chrono::milliseconds;

namespace {

constexpr int updated = 1 << 1;
constexpr int added = 1 << 2;
constexpr int deleted_filter = 1 << 3;

class FileEventServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::weakly_canonical(fs::temp_directory_path()) /
                ("anvil-file-event-test-" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_ / "dir");
        dir_ = root_ / "dir";
        session_ = std::make_unique<Session>("{}");
        providers_ = std::make_unique<FileSystemProviders>(*session_, router_, logger_);
        host_ = std::make_unique<FakeHost>(*session_);
        session_->set_request_handler([this](const Request& r) { return router_.dispatch(r); });
        host_->complete_handshake();
    }
    void TearDown() override {
        service_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    void create_service(std::optional<core::Workspace> workspace = std::nullopt) {
        service_ =
            std::make_unique<FileEventService>(*session_, router_, *providers_, logger_, workspace);
    }

    // Sends `$watch` (`session` and `correlate` as the extension host sends them).
    void watch(const fs::path& path, double session, json options = {{"recursive", true}},
               bool correlate = false, const std::string& scheme = "file") {
        if (!options.contains("excludes")) {
            options["excludes"] = json::array();
        }
        const json uri = {{"$mid", 1}, {"scheme", scheme}, {"path", path.generic_string()}};
        send("$watch", json::array({"acme.ext", session, uri, options, correlate}));
    }
    void unwatch(double session) {
        send("$unwatch", json::array({session}));
    }
    void send(const std::string& method, const json& args) {
        ASSERT_TRUE(host_
                        ->send_rpc(Request{
                            next_request_++,
                            static_cast<std::uint8_t>(ProxyId::MainThreadFileSystemEventService),
                            method, JsonArgs{args.dump()}})
                        .has_value());
        host_->drain_rpc();
    }

    static void write(const fs::path& path, const std::string& text) {
        std::ofstream(path, std::ios::app) << text;
    }

    // The batches `$onFileEvent` has been called with so far, collecting more for up to `wait`
    // or until `done` holds.
    template <class Done> bool wait_for(Done done, milliseconds wait = milliseconds(10000)) {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (!done(batches_) && std::chrono::steady_clock::now() < deadline) {
            service_->poll();
            for (const RpcMessage& message : host_->drain_rpc()) {
                const auto* request = std::get_if<Request>(&message);
                if (request && request->method == "$onFileEvent") {
                    EXPECT_EQ(request->proxy_id,
                              static_cast<std::uint8_t>(ProxyId::ExtHostFileSystemEventService));
                    batches_.push_back(json::parse(std::get<JsonArgs>(request->args).json)[0]);
                }
            }
            std::this_thread::sleep_for(milliseconds(5));
        }
        return done(batches_);
    }

    // The paths in the `kind` lists of all batches.
    std::vector<std::string> paths(const std::string& kind) const {
        std::vector<std::string> result;
        for (const json& batch : batches_) {
            for (const json& uri : batch[kind]) {
                result.push_back(uri["path"].get<std::string>());
            }
        }
        return result;
    }

    bool has(const std::string& kind, const fs::path& path) const {
        const auto found = paths(kind);
        return std::ranges::find(found, path.generic_string()) != found.end();
    }

    bool warned(const std::string& text) const {
        for (const auto& entry : logger_->entries()) {
            if (entry.level == core::LogLevel::warning &&
                entry.message.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    fs::path root_;
    fs::path dir_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<FakeHost> host_;
    RequestRouter router_;
    std::shared_ptr<core::RecordingLogger> logger_ = std::make_shared<core::RecordingLogger>();
    std::unique_ptr<FileSystemProviders> providers_;
    std::unique_ptr<FileEventService> service_;
    std::vector<json> batches_;
    std::uint32_t next_request_ = 1;
};

} // namespace

TEST_F(FileEventServiceTest, AWatchedFolderReportsCreatedChangedAndDeletedFiles) {
    create_service();
    watch(dir_, 0.25);

    write(dir_ / "a.txt", "x");
    ASSERT_TRUE(wait_for([&](auto&) { return has("created", dir_ / "a.txt"); }));
    EXPECT_FALSE(batches_[0].contains("session")) << "an uncorrelated batch has no session";
    EXPECT_EQ(batches_[0]["created"][0],
              (json{{"$mid", 1}, {"scheme", "file"}, {"path", (dir_ / "a.txt").generic_string()}}));

    write(dir_ / "a.txt", "y");
    ASSERT_TRUE(wait_for([&](auto&) { return has("changed", dir_ / "a.txt"); }));

    fs::remove(dir_ / "a.txt");
    ASSERT_TRUE(wait_for([&](auto&) { return has("deleted", dir_ / "a.txt"); }));
}

TEST_F(FileEventServiceTest, ABurstIsOneBatchInTheOrderOfTheEvents) {
    create_service();
    watch(dir_, 0.25);

    for (const char* name : {"one", "two", "three"}) {
        write(dir_ / name, "x");
    }
    ASSERT_TRUE(wait_for([&](auto&) { return paths("created").size() == 3; }));
    ASSERT_EQ(batches_.size(), 1u);
    EXPECT_EQ(paths("created"), (std::vector<std::string>{(dir_ / "one").generic_string(),
                                                          (dir_ / "two").generic_string(),
                                                          (dir_ / "three").generic_string()}));
}

TEST_F(FileEventServiceTest, ACorrelatedWatchCarriesItsSessionAndFilter) {
    create_service();
    watch(dir_, 0.5, {{"recursive", true}, {"filter", added | deleted_filter}}, true);

    write(dir_ / "a.txt", "x");
    ASSERT_TRUE(wait_for([&](auto&) { return has("created", dir_ / "a.txt"); }));
    ASSERT_EQ(batches_[0]["session"], 0.5);

    write(dir_ / "a.txt", "y");
    fs::remove(dir_ / "a.txt");
    ASSERT_TRUE(wait_for([&](auto&) { return has("deleted", dir_ / "a.txt"); }));
    EXPECT_TRUE(paths("changed").empty()) << "the filter leaves out the updates";
    for (const json& batch : batches_) {
        EXPECT_EQ(batch["session"], 0.5);
    }
}

TEST_F(FileEventServiceTest, AFilterThatKeepsUpdatesAloneLeavesOutCreations) {
    create_service();
    watch(dir_, 0.5, {{"recursive", true}, {"filter", updated}}, true);

    write(dir_ / "a.txt", "x");
    wait_for([&](auto&) { return false; }, milliseconds(300));
    write(dir_ / "a.txt", "y");
    ASSERT_TRUE(wait_for([&](auto&) { return has("changed", dir_ / "a.txt"); }));
    EXPECT_TRUE(paths("created").empty());
}

TEST_F(FileEventServiceTest, AnUnwatchedSessionStopsReporting) {
    create_service();
    watch(dir_, 0.25);
    unwatch(0.25);
    EXPECT_EQ(service_->watcher_count(), 0u);

    write(dir_ / "a.txt", "x");
    EXPECT_FALSE(wait_for([&](auto& batches) { return !batches.empty(); }, milliseconds(500)));
}

TEST_F(FileEventServiceTest, WatchesOfTheSameFolderShareOneWatcherAndReportEachChangeOnce) {
    fs::create_directories(dir_ / "inner");
    create_service();
    watch(dir_, 0.1);
    watch(dir_, 0.2);
    watch(dir_ / "inner", 0.3);
    EXPECT_EQ(service_->watcher_count(), 1u);

    write(dir_ / "inner" / "a.txt", "x");
    ASSERT_TRUE(wait_for([&](auto&) { return has("created", dir_ / "inner" / "a.txt"); }));
    wait_for([&](auto&) { return false; }, milliseconds(300));
    EXPECT_EQ(paths("created").size(), 1u);

    unwatch(0.1);
    unwatch(0.3);
    EXPECT_EQ(service_->watcher_count(), 1u) << "one session still uses the watcher";
    write(dir_ / "b.txt", "x");
    EXPECT_TRUE(wait_for([&](auto&) { return has("created", dir_ / "b.txt"); }));

    unwatch(0.2);
    EXPECT_EQ(service_->watcher_count(), 0u);
}

TEST_F(FileEventServiceTest, AWatchInsideTheWorkspaceUsesTheWatcherOfTheWorkspace) {
    fs::create_directories(dir_ / "inner");
    const auto workspace = core::Workspace::open(dir_);
    ASSERT_TRUE(workspace.has_value());
    create_service(*workspace);
    watch(dir_ / "inner", 0.1);
    watch(dir_, 0.2, {{"recursive", true}, {"excludes", json::array({"x/**"})}});
    EXPECT_EQ(service_->watcher_count(), 1u);

    write(dir_ / "inner" / "a.txt", "x");
    ASSERT_TRUE(wait_for([&](auto&) { return has("created", dir_ / "inner" / "a.txt"); }));
    wait_for([&](auto&) { return false; }, milliseconds(300));
    EXPECT_EQ(paths("created").size(), 1u);
}

TEST_F(FileEventServiceTest, AWatchOfAFolderThatIsNotCoveredGetsAWatcherOfItsOwn) {
    fs::create_directories(dir_ / "inner");
    create_service();
    watch(dir_, 0.1, {{"recursive", false}});
    watch(dir_ / "inner", 0.2, {{"recursive", false}});
    watch(dir_, 0.3, {{"recursive", true}});
    EXPECT_EQ(service_->watcher_count(), 3u);
}

TEST_F(FileEventServiceTest, ExcludesAreRelativeToTheWatchedFolderOrAbsolute) {
    fs::create_directories(dir_ / "skip");
    fs::create_directories(dir_ / "abs");
    create_service();
    watch(dir_, 0.25,
          {{"recursive", true},
           {"excludes", json::array({"skip/**", (dir_ / "abs").generic_string()})}});

    write(dir_ / "skip" / "a.txt", "x");
    write(dir_ / "abs" / "a.txt", "x");
    write(dir_ / "seen.txt", "x");
    ASSERT_TRUE(wait_for([&](auto&) { return has("created", dir_ / "seen.txt"); }));
    EXPECT_FALSE(has("created", dir_ / "skip" / "a.txt"));
    EXPECT_FALSE(has("created", dir_ / "abs" / "a.txt"));
}

TEST_F(FileEventServiceTest, AWatchThatCannotStartIsLoggedAndLeavesTheOthersWorking) {
    create_service();
    watch(dir_ / "missing", 0.1);
    EXPECT_TRUE(warned("missing")) << "the watch of a folder that does not exist is logged";
    watch(dir_, 0.2);

    write(dir_ / "a.txt", "x");
    EXPECT_TRUE(wait_for([&](auto&) { return has("created", dir_ / "a.txt"); }));
}

TEST_F(FileEventServiceTest, AResourceThatIsNotAFileIsLoggedAndNotWatched) {
    create_service();
    watch(dir_, 0.1, {{"recursive", true}}, false, "vscode-vfs");
    EXPECT_EQ(service_->watcher_count(), 0u);
    EXPECT_TRUE(warned("not a file"));
}

TEST_F(FileEventServiceTest, TheWorkspaceFolderIsWatchedWithoutAnyWatch) {
    fs::create_directories(dir_ / ".git" / "objects");
    const auto workspace = core::Workspace::open(dir_);
    ASSERT_TRUE(workspace.has_value());
    create_service(*workspace);
    EXPECT_EQ(service_->watcher_count(), 1u);

    write(dir_ / ".git" / "objects" / "pack", "x");
    write(dir_ / "a.txt", "x");
    ASSERT_TRUE(wait_for([&](auto&) { return has("created", dir_ / "a.txt"); }));
    EXPECT_FALSE(has("created", dir_ / ".git" / "objects" / "pack"))
        << "files.watcherExclude leaves out the objects of Git";
}

TEST_F(FileEventServiceTest, TheWorkspaceWatcherStaysWhenAnExtensionUnwatchesTheSameFolder) {
    const auto workspace = core::Workspace::open(dir_);
    ASSERT_TRUE(workspace.has_value());
    create_service(*workspace);
    watch(dir_, 0.25, {{"recursive", true}, {"excludes", json::array({".git/objects/**"})}});
    EXPECT_EQ(service_->watcher_count(), 1u);
    unwatch(0.25);
    EXPECT_EQ(service_->watcher_count(), 1u);

    write(dir_ / "a.txt", "x");
    EXPECT_TRUE(wait_for([&](auto&) { return has("created", dir_ / "a.txt"); }));
}

// A provider that is registered for a scheme: watching its resources is the provider's job.
TEST_F(FileEventServiceTest, AWatchOfAProvidedResourceIsPassedToTheProviderAndEndsWithIt) {
    create_service();
    ASSERT_TRUE(host_
                    ->send_rpc(Request{next_request_++,
                                       static_cast<std::uint8_t>(ProxyId::MainThreadFileSystem),
                                       "$registerFileSystemProvider", JsonArgs{R"([3,"memfs",2])"}})
                    .has_value());
    host_->drain_rpc();

    const json resource = {{"$mid", 1}, {"scheme", "memfs"}, {"path", "/dir"}};
    ASSERT_TRUE(
        host_
            ->send_rpc(Request{
                next_request_++,
                static_cast<std::uint8_t>(ProxyId::MainThreadFileSystemEventService), "$watch",
                JsonArgs{json::array({"acme.ext",
                                      0.75,
                                      resource,
                                      {{"recursive", true}, {"excludes", json::array()}},
                                      false})
                             .dump()}})
            .has_value());
    std::vector<Request> provider_calls;
    for (const RpcMessage& message : host_->drain_rpc()) {
        if (const auto* request = std::get_if<Request>(&message)) {
            provider_calls.push_back(*request);
        }
    }
    ASSERT_EQ(provider_calls.size(), 1u);
    EXPECT_EQ(provider_calls[0].proxy_id, static_cast<std::uint8_t>(ProxyId::ExtHostFileSystem));
    EXPECT_EQ(provider_calls[0].method, "$watch");
    const auto& args = std::get<MixedArgs>(provider_calls[0].args);
    EXPECT_EQ(json::parse(std::get<JsonArg>(args[0]).json), 3);
    EXPECT_EQ(json::parse(std::get<JsonArg>(args[1]).json), 0.75);
    EXPECT_EQ(service_->watcher_count(), 0u) << "the editor watches no folder for it";

    ASSERT_TRUE(
        host_
            ->send_rpc(Request{next_request_++,
                               static_cast<std::uint8_t>(ProxyId::MainThreadFileSystemEventService),
                               "$unwatch", JsonArgs{"[0.75]"}})
            .has_value());
    provider_calls.clear();
    for (const RpcMessage& message : host_->drain_rpc()) {
        if (const auto* request = std::get_if<Request>(&message)) {
            provider_calls.push_back(*request);
        }
    }
    ASSERT_EQ(provider_calls.size(), 1u);
    EXPECT_EQ(provider_calls[0].method, "$unwatch");
}

TEST_F(FileEventServiceTest, ChangesThatAProviderReportsAreSentAsFileEvents) {
    create_service();
    host_->send_rpc(Request{next_request_++,
                            static_cast<std::uint8_t>(ProxyId::MainThreadFileSystem),
                            "$registerFileSystemProvider", JsonArgs{R"([3,"memfs",2])"}});
    host_->drain_rpc();

    const auto uri = [](const std::string& path) {
        return json{{"$mid", 1}, {"scheme", "memfs"}, {"path", path}};
    };
    host_->send_rpc(
        Request{next_request_++, static_cast<std::uint8_t>(ProxyId::MainThreadFileSystem),
                "$onFileSystemChange",
                JsonArgs{json::array({3, json::array({{{"resource", uri("/new")}, {"type", 1}},
                                                      {{"resource", uri("/edited")}, {"type", 0}},
                                                      {{"resource", uri("/gone")}, {"type", 2}},
                                                      {{"resource", uri("/new2")}, {"type", 1}}})})
                             .dump()}});
    std::vector<json> batches;
    for (const RpcMessage& message : host_->drain_rpc()) {
        const auto* request = std::get_if<Request>(&message);
        if (request && request->method == "$onFileEvent") {
            batches.push_back(json::parse(std::get<JsonArgs>(request->args).json)[0]);
        }
    }
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_FALSE(batches[0].contains("session"));
    EXPECT_EQ(batches[0]["created"], json::array({uri("/new"), uri("/new2")}));
    EXPECT_EQ(batches[0]["changed"], json::array({uri("/edited")}));
    EXPECT_EQ(batches[0]["deleted"], json::array({uri("/gone")}));
}
