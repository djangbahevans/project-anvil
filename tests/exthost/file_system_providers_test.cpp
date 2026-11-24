#include "anvil/exthost/file_system_providers.hpp"
#include "anvil/exthost/vscode_types.hpp"
#include "fake_host.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace anvil;
using namespace anvil::exthost;
using anvil::exthost::test::FakeHost;
using nlohmann::json;

namespace {

constexpr int read_write = 2;
constexpr int readonly = 1 << 11;
constexpr int folder_copy = 1 << 3;

core::Uri memfs(const std::string& path, const std::string& scheme = "memfs") {
    return core::Uri{scheme, "", path, "", ""};
}

// A call that the editor made to the extension host.
struct Call {
    std::uint32_t req = 0;
    std::string method;
    MixedArgs args;
};

class FileSystemProvidersTest : public ::testing::Test {
protected:
    FileSystemProvidersTest()
        : session_("{}"), host_(session_), providers_(session_, router_, logger_) {
        session_.set_request_handler([this](const Request& r) { return router_.dispatch(r); });
        host_.complete_handshake();
    }

    // A request from the extension host to the editor.
    std::vector<RpcMessage> send(const std::string& method, const json& args) {
        EXPECT_TRUE(
            host_
                .send_rpc(Request{next_++, static_cast<std::uint8_t>(ProxyId::MainThreadFileSystem),
                                  method, JsonArgs{args.dump()}})
                .has_value());
        return host_.drain_rpc();
    }
    void register_provider(double handle, const std::string& scheme, int capabilities) {
        send("$registerFileSystemProvider", json::array({handle, scheme, capabilities}));
    }

    // The calls to providers that the editor has made since the last time.
    std::vector<Call> calls() {
        std::vector<Call> found;
        for (const RpcMessage& message : host_.drain_rpc()) {
            const auto* request = std::get_if<Request>(&message);
            if (request &&
                request->proxy_id == static_cast<std::uint8_t>(ProxyId::ExtHostFileSystem)) {
                found.push_back(
                    {request->req, request->method, std::get<MixedArgs>(request->args)});
            }
        }
        return found;
    }
    static json json_of(const MixedArg& arg) {
        return json::parse(std::get<JsonArg>(arg).json);
    }

    void reply_json(const Call& call, const json& value) {
        ASSERT_TRUE(host_.send_rpc(JsonReply{call.req, value.dump()}).has_value());
    }
    void reply_empty(const Call& call) {
        ASSERT_TRUE(host_.send_rpc(EmptyReply{call.req}).has_value());
    }
    void reply_error(const Call& call, const std::string& name, const std::string& message) {
        ASSERT_TRUE(host_
                        .send_rpc(ErrorReply{
                            call.req,
                            json{{"$isError", true}, {"name", name}, {"message", message}}.dump()})
                        .has_value());
    }

    Session session_;
    FakeHost host_;
    RequestRouter router_;
    std::shared_ptr<core::RecordingLogger> logger_ = std::make_shared<core::RecordingLogger>();
    FileSystemProviders providers_;
    std::uint32_t next_ = 1;
};

} // namespace

TEST_F(FileSystemProvidersTest, ProvidersAreRegisteredByScheme) {
    register_provider(3, "memfs", read_write);
    ASSERT_NE(providers_.find("memfs"), nullptr);
    EXPECT_EQ(providers_.find("memfs")->handle, 3);
    EXPECT_EQ(providers_.find("memfs")->capabilities, read_write);
    EXPECT_EQ(providers_.find("other"), nullptr);

    const auto replies = send("$registerFileSystemProvider", json::array({4, "memfs", read_write}));
    ASSERT_EQ(replies.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<ErrorReply>(replies[1])) << "a scheme has one provider";
    EXPECT_EQ(providers_.find("memfs")->handle, 3);

    send("$unregisterProvider", json::array({3}));
    EXPECT_EQ(providers_.find("memfs"), nullptr);
    EXPECT_EQ(providers_.count(), 0u);
}

TEST_F(FileSystemProvidersTest, AnOperationCallsTheProviderOfTheSchemeWithItsHandle) {
    register_provider(3, "memfs", read_write);
    register_provider(7, "other", read_write);

    std::optional<std::expected<ProviderStat, ProviderError>> result;
    providers_.stat(memfs("/a.txt", "other"), [&](auto r) { result = std::move(r); });
    const auto sent = calls();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].method, "$stat");
    ASSERT_EQ(sent[0].args.size(), 2u);
    EXPECT_EQ(json_of(sent[0].args[0]), 7);
    EXPECT_EQ(json_of(sent[0].args[1])["path"], "/a.txt");
    EXPECT_EQ(json_of(sent[0].args[1])["scheme"], "other");
    EXPECT_FALSE(result.has_value());

    reply_json(sent[0], {{"type", 1}, {"ctime", 5}, {"mtime", 6}, {"size", 42}});
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->type, 1);
    EXPECT_EQ((*result)->mtime, 6);
    EXPECT_EQ((*result)->size, 42);
}

TEST_F(FileSystemProvidersTest, ReadDirectoryAndReadFileGiveTheirResults) {
    register_provider(3, "memfs", read_write);

    std::optional<std::expected<std::vector<std::pair<std::string, int>>, ProviderError>> listing;
    providers_.read_directory(memfs("/"), [&](auto r) { listing = std::move(r); });
    reply_json(calls().at(0), json::array({json::array({"a", 1}), json::array({"b", 2})}));
    ASSERT_TRUE(listing && listing->has_value());
    EXPECT_EQ(**listing, (std::vector<std::pair<std::string, int>>{{"a", 1}, {"b", 2}}));

    std::optional<std::expected<Bytes, ProviderError>> content;
    providers_.read_file(memfs("/a"), [&](auto r) { content = std::move(r); });
    const Call call = calls().at(0);
    EXPECT_EQ(call.method, "$readFile");
    ASSERT_TRUE(
        host_.send_rpc(BufferReply{call.req, to_bytes(std::string("a\0b", 3))}).has_value());
    ASSERT_TRUE(content && content->has_value());
    EXPECT_EQ(**content, to_bytes(std::string("a\0b", 3)));
}

TEST_F(FileSystemProvidersTest, WriteFileSendsTheContentAsABufferWithTheOptions) {
    register_provider(3, "memfs", read_write);

    std::optional<std::expected<std::monostate, ProviderError>> result;
    providers_.write_file(memfs("/a"), to_bytes("hello"), [&](auto r) { result = std::move(r); });
    const Call call = calls().at(0);
    EXPECT_EQ(call.method, "$writeFile");
    ASSERT_EQ(call.args.size(), 4u);
    EXPECT_EQ(json_of(call.args[0]), 3);
    EXPECT_EQ(std::get<BufferArg>(call.args[2]).data, to_bytes("hello"));
    EXPECT_EQ(json_of(call.args[3]),
              json::parse(R"({"create":true,"overwrite":true,"unlock":false,"atomic":false})"));

    reply_empty(call);
    ASSERT_TRUE(result && result->has_value());
}

TEST_F(FileSystemProvidersTest, RenameCopyMkdirAndDeleteCallWithTheirOptions) {
    register_provider(3, "memfs", read_write | folder_copy);

    providers_.rename(memfs("/a"), memfs("/b"), true, [](auto) {});
    Call call = calls().at(0);
    EXPECT_EQ(call.method, "$rename");
    EXPECT_EQ(json_of(call.args[2])["path"], "/b");
    EXPECT_EQ(json_of(call.args[3]), json::parse(R"({"overwrite":true})"));

    providers_.copy(memfs("/a"), memfs("/c"), false, [](auto) {});
    call = calls().at(0);
    EXPECT_EQ(call.method, "$copy");
    EXPECT_EQ(json_of(call.args[3]), json::parse(R"({"overwrite":false})"));

    providers_.create_directory(memfs("/d"), [](auto) {});
    call = calls().at(0);
    EXPECT_EQ(call.method, "$mkdir");
    EXPECT_EQ(call.args.size(), 2u);

    providers_.remove(memfs("/d"), true, [](auto) {});
    call = calls().at(0);
    EXPECT_EQ(call.method, "$delete");
    EXPECT_EQ(json_of(call.args[2]),
              json::parse(R"({"recursive":true,"useTrash":false,"atomic":false})"));
}

TEST_F(FileSystemProvidersTest, AProviderErrorKeepsItsCode) {
    register_provider(3, "memfs", read_write);

    std::optional<std::expected<ProviderStat, ProviderError>> result;
    providers_.stat(memfs("/gone"), [&](auto r) { result = std::move(r); });
    reply_error(calls().at(0), "EntryNotFound (FileSystemError)", "gone");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().name, "EntryNotFound");
    EXPECT_EQ(result->error().message, "gone");

    providers_.stat(memfs("/x"), [&](auto r) { result = std::move(r); });
    reply_error(calls().at(0), "Error", "boom");
    EXPECT_EQ(result->error().name, "Unknown");

    providers_.stat(memfs("/y"), [&](auto r) { result = std::move(r); });
    reply_error(calls().at(0), "ENOPRO", "no provider");
    EXPECT_EQ(result->error().name, "Unavailable");
}

TEST_F(FileSystemProvidersTest, ASchemeWithoutProviderIsUnavailableWithoutACall) {
    std::optional<std::expected<ProviderStat, ProviderError>> result;
    providers_.stat(memfs("/a"), [&](auto r) { result = std::move(r); });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().name, "Unavailable");
    EXPECT_TRUE(calls().empty());
}

TEST_F(FileSystemProvidersTest, AReadOnlyProviderRefusesChangesWithoutBeingCalled) {
    register_provider(3, "memfs", read_write | readonly | folder_copy);

    std::vector<std::string> failures;
    const auto record = [&](std::expected<std::monostate, ProviderError> r) {
        failures.push_back(r ? "ok" : r.error().name);
    };
    providers_.write_file(memfs("/a"), to_bytes("x"), record);
    providers_.rename(memfs("/a"), memfs("/b"), false, record);
    providers_.copy(memfs("/a"), memfs("/b"), false, record);
    providers_.create_directory(memfs("/d"), record);
    providers_.remove(memfs("/a"), false, record);
    EXPECT_EQ(failures, std::vector<std::string>(5, "NoPermissions"));
    EXPECT_TRUE(calls().empty());

    providers_.stat(memfs("/a"), [](auto) {});
    EXPECT_EQ(calls().size(), 1u) << "reading is allowed";
}

TEST_F(FileSystemProvidersTest, ARenameOrCopyBetweenSchemesIsUnavailable) {
    register_provider(3, "memfs", read_write | folder_copy);
    register_provider(7, "other", read_write | folder_copy);

    std::vector<std::string> failures;
    const auto record = [&](std::expected<std::monostate, ProviderError> r) {
        failures.push_back(r ? "ok" : r.error().name);
    };
    providers_.rename(memfs("/a"), memfs("/a", "other"), false, record);
    providers_.copy(memfs("/a"), memfs("/a", "other"), false, record);
    EXPECT_EQ(failures, (std::vector<std::string>{"Unavailable", "Unavailable"}));
    EXPECT_TRUE(calls().empty());
}

TEST_F(FileSystemProvidersTest, ACopyNeedsTheCapabilityOfTheProvider) {
    register_provider(3, "memfs", read_write);
    std::optional<std::expected<std::monostate, ProviderError>> result;
    providers_.copy(memfs("/a"), memfs("/b"), false, [&](auto r) { result = std::move(r); });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().name, "Unavailable");
    EXPECT_TRUE(calls().empty());
}

TEST_F(FileSystemProvidersTest, ChangesThatAProviderReportsGoToTheListenerInOrder) {
    register_provider(3, "memfs", read_write);
    std::vector<ProviderChange> seen;
    providers_.set_change_listener([&](const std::vector<ProviderChange>& changes) {
        seen.insert(seen.end(), changes.begin(), changes.end());
    });

    send("$onFileSystemChange",
         json::array({3, json::array({{{"resource", uri_to_json(memfs("/a"))}, {"type", 1}},
                                      {{"resource", uri_to_json(memfs("/b"))}, {"type", 0}},
                                      {{"resource", uri_to_json(memfs("/c"))}, {"type", 2}}})}));
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0].resource.path, "/a");
    EXPECT_EQ(seen[0].type, 1);
    EXPECT_EQ(seen[1].type, 0);
    EXPECT_EQ(seen[2].resource.path, "/c");
    EXPECT_EQ(seen[2].type, 2);
}

TEST_F(FileSystemProvidersTest, WatchAndUnwatchGoToTheProviderWithTheSession) {
    register_provider(3, "memfs", read_write);

    EXPECT_FALSE(providers_.watch(memfs("/a", "nothing"), 0.5, json::object()));
    EXPECT_TRUE(
        providers_.watch(memfs("/a"), 0.5, {{"recursive", true}, {"excludes", json::array()}}));
    Call call = calls().at(0);
    EXPECT_EQ(call.method, "$watch");
    ASSERT_EQ(call.args.size(), 4u);
    EXPECT_EQ(json_of(call.args[0]), 3);
    EXPECT_EQ(json_of(call.args[1]), 0.5);
    EXPECT_EQ(json_of(call.args[3])["recursive"], true);

    providers_.unwatch("memfs", 0.5);
    call = calls().at(0);
    EXPECT_EQ(call.method, "$unwatch");
    EXPECT_EQ(json_of(call.args[1]), 0.5);
}
