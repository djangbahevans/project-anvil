#include "anvil/exthost/vscode_types.hpp"
#include "anvil/exthost/workspace_service.hpp"
#include "fake_host.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

using namespace anvil::exthost;
using anvil::core::FileSearchOptions;
using anvil::core::LogLevel;
using anvil::core::RecordingLogger;
using anvil::exthost::test::FakeHost;
using nlohmann::json;
using std::chrono::milliseconds;
namespace fs = std::filesystem;
namespace core = anvil::core;

namespace {

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

class WorkspaceServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("anvil-workspace-service-test-" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_ / "project");
        root_ = fs::canonical(root_);
    }
    void TearDown() override {
        service_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    void write(const std::string& name, const std::string& contents = "x") {
        const fs::path path = root_ / name;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }
    // A shell script in place of ripgrep.
    fs::path program(const std::string& script) {
        write("stand-in/rg", "#!/bin/sh\n" + script + "\n");
        fs::permissions(root_ / "stand-in/rg", fs::perms::owner_all);
        return root_ / "stand-in/rg";
    }

    void serve(std::optional<fs::path> ripgrep, const std::string& settings = "{}") {
        service_ = std::make_unique<WorkspaceService>(
            session_, router_, logger_,
            SearchSettings{.ripgrep = std::move(ripgrep),
                           .options =
                               FileSearchOptions::from_settings(*core::Settings::parse(settings))});
    }

    json uri(const std::string& folder) const {
        return uri_to_json(core::Uri::file(root_ / folder));
    }
    // Sends `$checkExists`, and returns what the session wrote at once.
    std::vector<RpcMessage> send(const json& folders, const json& includes) {
        const Request request{++req_, static_cast<std::uint8_t>(ProxyId::MainThreadWorkspace),
                              "$checkExists", JsonArgs{json::array({folders, includes}).dump()},
                              true};
        EXPECT_TRUE(host_.send_rpc(request).has_value());
        return host_.drain_rpc();
    }
    static std::optional<bool> answer_in(const std::vector<RpcMessage>& messages,
                                         std::uint32_t req) {
        for (const RpcMessage& message : messages) {
            if (const auto* reply = std::get_if<JsonReply>(&message); reply && reply->req == req) {
                return json::parse(reply->json).get<bool>();
            }
        }
        return std::nullopt;
    }
    // The answer written as soon as the request arrives. `send` moves `req_` on, so it is a
    // statement of its own: the order in which arguments are evaluated differs between compilers.
    std::optional<bool> answer_at_once(const json& folders, const json& includes) {
        const auto messages = send(folders, includes);
        return answer_in(messages, req_);
    }
    // The answer to the request sent last, polling for up to ten seconds.
    std::optional<bool> reply_to(std::vector<RpcMessage> messages) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto answer = answer_in(messages, req_)) {
                return answer;
            }
            std::this_thread::sleep_for(milliseconds(2));
            service_->poll();
            messages = host_.drain_rpc();
        }
        return std::nullopt;
    }
    std::optional<bool> check(const json& folders, const json& includes) {
        return reply_to(send(folders, includes));
    }
    // Whether a file matching `pattern` is in the project.
    std::optional<bool> exists(const std::string& pattern) {
        return check(json::array({uri("project")}), json::array({pattern}));
    }
    pid_t written_pid() const {
        for (int i = 0; i < 500; ++i) {
            // The script runs in the folder that is searched.
            std::ifstream in(root_ / "project" / "pid");
            pid_t pid = 0;
            if (in >> pid) {
                return pid;
            }
            std::this_thread::sleep_for(milliseconds(10));
        }
        return 0;
    }
    std::size_t warnings(const std::string& text) const {
        std::size_t count = 0;
        for (const auto& entry : logger_->entries()) {
            count +=
                entry.level == LogLevel::warning && entry.message.find(text) != std::string::npos
                    ? 1
                    : 0;
        }
        return count;
    }

    fs::path root_;
    std::shared_ptr<RecordingLogger> logger_ = std::make_shared<RecordingLogger>();
    Session session_{"{}"};
    RequestRouter router_;
    FakeHost host_{session_};
    std::unique_ptr<WorkspaceService> service_;
    std::uint32_t req_ = 0;

public:
    WorkspaceServiceTest() {
        session_.set_request_handler(
            [this](const Request& request) { return router_.dispatch(request); });
        host_.complete_handshake();
    }
};

// With the real ripgrep, which CI requires.
class WorkspaceSearchTest : public WorkspaceServiceTest {
protected:
    void SetUp() override {
        WorkspaceServiceTest::SetUp();
        ripgrep_ = anvil::search::find_ripgrep({.override_path = env("ANVIL_RG"),
                                                .vscode_dir = ANVIL_VSCODE_DIR,
                                                .path_variable = env("PATH")});
        if (!ripgrep_ || ::access(ripgrep_->c_str(), X_OK) != 0) {
            ASSERT_TRUE(env("ANVIL_REQUIRE_RIPGREP").empty()) << "ripgrep is required and missing";
            GTEST_SKIP() << "needs ripgrep (scripts/fetch-vscode.sh, PATH or ANVIL_RG)";
        }
        serve(ripgrep_);
    }
    std::optional<fs::path> ripgrep_;
};

} // namespace

TEST_F(WorkspaceSearchTest, AFileThatMatchesAtAnyDepthExistsAndOneThatDoesNotMatchIsNone) {
    write("project/a/b/c/deep.fixture");
    write("project/top.txt");
    EXPECT_EQ(exists("**/*.fixture"), true);
    EXPECT_EQ(exists("**/*.toml"), false);
    // A pattern without `**` is relative to the folder, as in VS Code.
    EXPECT_EQ(exists("*.txt"), true);
    EXPECT_EQ(exists("*.fixture"), false);
    EXPECT_EQ(service_->pending(), 0U);
}

TEST_F(WorkspaceSearchTest, WhatTheSearchRulesLeaveOutDoesNotCount) {
    write("project/node_modules/dep/x.fixture");
    EXPECT_EQ(exists("**/*.fixture"), false);
    write("project/dist/y.fixture");
    EXPECT_EQ(exists("**/*.fixture"), true);
    serve(ripgrep_, R"({"search.exclude": {"**/dist": true}})");
    EXPECT_EQ(exists("**/*.fixture"), false);
}

TEST_F(WorkspaceSearchTest, AnIgnoredFileThatThePatternNamesCountsButNotOneInAnIgnoredFolder) {
    write("project/.gitignore", "build/\nlocal.fixture\n");
    write("project/build/out.fixture");
    EXPECT_EQ(exists("**/*.fixture"), false);
    write("project/local.fixture");
    EXPECT_EQ(exists("**/*.fixture"), true);
}

TEST_F(WorkspaceSearchTest, ADirectoryNamedLikeThePatternIsNoFile) {
    write("project/folder.fixture/inside.txt");
    EXPECT_EQ(exists("**/*.fixture"), false);
}

TEST_F(WorkspaceSearchTest, AnyOfThePatternsInAnyOfTheFoldersDoes) {
    write("project/a.txt");
    write("other/b.toml");
    const json folders = json::array({uri("project"), uri("missing"), uri("other")});
    EXPECT_EQ(check(folders, json::array({"**/*.fixture", "**/*.toml"})), true);
    EXPECT_EQ(check(folders, json::array({"**/*.fixture"})), false);
    EXPECT_EQ(warnings("missing"), 2U);
}

TEST_F(WorkspaceSearchTest, FoldersThatAreNotOnThisMachineAreNotSearched) {
    write("project/a.fixture");
    json remote = uri("project");
    remote["scheme"] = "memfs";
    EXPECT_EQ(check(json::array({remote}), json::array({"**/*.fixture"})), false);
    EXPECT_EQ(check(json::array({remote, uri("project")}), json::array({"**/*.fixture"})), true);
}

TEST_F(WorkspaceServiceTest, ArgumentsThatNameNothingToSearchForAreFalseAtOnce) {
    serve(program("echo $$ > pid; exec sleep 30"));
    const json folders = json::array({uri("project")});
    EXPECT_EQ(answer_at_once(folders, json::array()), false);
    EXPECT_EQ(answer_at_once(folders, json::array({"", 7})), false);
    EXPECT_EQ(answer_at_once(json::array(), json::array({"**"})), false);
    EXPECT_EQ(answer_at_once("folders", json::array({"**"})), false);
    EXPECT_FALSE(fs::exists(root_ / "project" / "pid"));
}

TEST_F(WorkspaceServiceTest, TheFirstFileEndsTheSearch) {
    serve(program("echo $$ > pid; printf 'one\\0'; exec sleep 30"));
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(exists("**/*"), true);
    EXPECT_LT(std::chrono::steady_clock::now() - start, milliseconds(4000));
    const pid_t pid = written_pid();
    ASSERT_GT(pid, 0);
    EXPECT_EQ(::kill(pid, 0), -1) << "ripgrep must not go on";
}

TEST_F(WorkspaceServiceTest, ASearchRunsWithTheRulesAndThePatterns) {
    serve(program("printf '%s\\n' \"$@\" > arguments; exit 1"),
          R"({"search.exclude": {"**/dist": true}})");
    EXPECT_EQ(exists("**/*.toml"), false);
    std::ifstream in(root_ / "project" / "arguments");
    const std::string arguments((std::istreambuf_iterator<char>(in)), {});
    EXPECT_NE(arguments.find("-g\n**/*.toml\n"), std::string::npos) << arguments;
    EXPECT_NE(arguments.find("-g\n!**/dist\n"), std::string::npos) << arguments;
}

TEST_F(WorkspaceServiceTest, ASearchThatTakesTooLongIsGivenUp) {
    serve(program("echo $$ > pid; exec sleep 30"));
    service_->set_time_limit(milliseconds(300));
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(exists("**/*.toml"), false);
    EXPECT_GE(std::chrono::steady_clock::now() - start, milliseconds(300));
    EXPECT_LT(std::chrono::steady_clock::now() - start, milliseconds(5000));
    const pid_t pid = written_pid();
    ASSERT_GT(pid, 0);
    EXPECT_EQ(::kill(pid, 0), -1);
    EXPECT_EQ(warnings("gave up searching for **/*.toml"), 1U);
    EXPECT_EQ(WorkspaceService::default_time_limit, milliseconds(5000));
}

TEST_F(WorkspaceServiceTest, TheConnectionIsServedWhileASearchRuns) {
    serve(program("exec sleep 0.5"));
    router_.on(
        ProxyId::MainThreadWindow, "$getInitialState",
        [](const Request&, const json&) -> std::optional<HandlerResult> { return OkJson{"{}"}; });
    const auto first = send(json::array({uri("project")}), json::array({"**/*.toml"}));
    EXPECT_FALSE(answer_in(first, req_).has_value());
    const std::uint32_t search = req_;
    const auto polled = std::chrono::steady_clock::now();
    service_->poll();
    EXPECT_LT(std::chrono::steady_clock::now() - polled, milliseconds(100))
        << "poll must not wait for the search";

    const Request other{++req_, static_cast<std::uint8_t>(ProxyId::MainThreadWindow),
                        "$getInitialState", JsonArgs{"[]"}};
    ASSERT_TRUE(host_.send_rpc(other).has_value());
    const auto messages = host_.drain_rpc();
    EXPECT_TRUE(std::ranges::any_of(messages, [&](const RpcMessage& message) {
        const auto* reply = std::get_if<JsonReply>(&message);
        return reply && reply->req == req_;
    }));
    EXPECT_FALSE(answer_in(messages, search).has_value());
    req_ = search;
    EXPECT_EQ(reply_to({}), false);
}

TEST_F(WorkspaceServiceTest, StoppingAndDestroyingEndRipgrep) {
    serve(program("echo $$ > pid; exec sleep 30"));
    send(json::array({uri("project")}), json::array({"**/*.toml"}));
    pid_t pid = written_pid();
    ASSERT_GT(pid, 0);
    service_->stop();
    EXPECT_EQ(::kill(pid, 0), -1);
    EXPECT_EQ(service_->pending(), 0U);

    fs::remove(root_ / "project" / "pid");
    send(json::array({uri("project")}), json::array({"**/*.toml"}));
    pid = written_pid();
    ASSERT_GT(pid, 0);
    service_.reset();
    EXPECT_EQ(::kill(pid, 0), -1);
}

TEST_F(WorkspaceServiceTest, WithoutRipgrepItIsFalseAndSaidOnce) {
    serve(std::nullopt);
    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(answer_at_once(json::array({uri("project")}), json::array({"**/*"})), false);
    }
    EXPECT_EQ(warnings("ripgrep, which searches for them, was not found"), 1U);
}

TEST_F(WorkspaceServiceTest, ASearchThatFailsIsLogged) {
    serve(program("echo 'cannot look' >&2; exit 2"));
    EXPECT_EQ(exists("**/*"), false);
    EXPECT_EQ(warnings("failed: cannot look"), 1U);
    serve(root_ / "no-such-rg");
    EXPECT_EQ(exists("**/*"), false);
    EXPECT_EQ(warnings("cannot search"), 1U);
}
