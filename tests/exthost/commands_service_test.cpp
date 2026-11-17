#include "anvil/exthost/commands_service.hpp"
#include "fake_host.hpp"

#include <gtest/gtest.h>

using namespace anvil::exthost;
using anvil::exthost::test::FakeHost;
using nlohmann::json;

namespace {

// A session in the running state with a commands service attached.
class CommandsServiceTest : public ::testing::Test {
protected:
    CommandsServiceTest() : session_("{}"), host_(session_), commands_(session_, router_) {
        session_.set_request_handler(
            [this](const Request& request) { return router_.dispatch(request); });
        host_.complete_handshake();
    }

    // Sends a request from the extension host and returns what the session wrote in response.
    std::vector<RpcMessage> request(std::string method, std::string args, std::uint32_t req = 1) {
        EXPECT_TRUE(
            host_
                .send_rpc(Request{req, static_cast<std::uint8_t>(ProxyId::MainThreadCommands),
                                  std::move(method), JsonArgs{std::move(args)}})
                .has_value());
        return host_.drain_rpc();
    }

    Session session_;
    FakeHost host_;
    RequestRouter router_;
    CommandsService commands_;
};

std::string reply_json(const RpcMessage& message) {
    return std::get<JsonReply>(message).json;
}

} // namespace

TEST_F(CommandsServiceTest, TracksCommandsRegisteredByExtensions) {
    request("$registerCommand", R"(["ext.a"])");
    request("$registerCommand", R"(["ext.b"])");
    EXPECT_EQ(commands_.extension_commands(), (std::set<std::string>{"ext.a", "ext.b"}));

    request("$unregisterCommand", R"(["ext.a"])");
    EXPECT_EQ(commands_.extension_commands(), (std::set<std::string>{"ext.b"}));
}

TEST_F(CommandsServiceTest, GetCommandsListsBuiltinAndExtensionCommandsOnce) {
    commands_.add_builtin("b.one", [](const json&) -> CallResult { return std::nullopt; });
    commands_.add_builtin("shared", [](const json&) -> CallResult { return std::nullopt; });
    request("$registerCommand", R"(["a.ext"])");
    request("$registerCommand", R"(["shared"])");

    const auto out = request("$getCommands", "[]");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(json::parse(reply_json(out[1])), json::parse(R"(["a.ext","b.one","shared"])"));
}

TEST_F(CommandsServiceTest, ExecutesBuiltinCommandsForTheExtensionHost) {
    json seen;
    commands_.add_builtin("b.double", [&](const json& args) -> CallResult {
        seen = args;
        return std::to_string(args.at(0).get<int>() * 2);
    });

    const auto out = request("$executeCommand", R"(["b.double",[21],true])");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(reply_json(out[1]), "42");
    EXPECT_EQ(seen, json::parse("[21]"));
}

TEST_F(CommandsServiceTest, UndefinedBuiltinResultIsAnEmptyReply) {
    commands_.add_builtin("b.void", [](const json&) -> CallResult { return std::nullopt; });
    const auto out = request("$executeCommand", R"(["b.void",[],false])");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1], RpcMessage{EmptyReply{1}});
}

TEST_F(CommandsServiceTest, BuiltinFailureBecomesAnErrorReply) {
    commands_.add_builtin(
        "b.fail", [](const json&) -> CallResult { return std::unexpected(error_json("nope")); });
    const auto out = request("$executeCommand", R"(["b.fail",[],false])");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(json::parse(std::get<ErrorReply>(out[1]).json)["message"], "nope");
}

TEST_F(CommandsServiceTest, UnknownCommandIsReportedAsNotFound) {
    const auto out = request("$executeCommand", R"(["no.such",[],false])");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_NE(std::get<ErrorReply>(out[1]).json.find("command 'no.such' not found"),
              std::string::npos);
}

TEST_F(CommandsServiceTest, UnknownCommandWithArgumentsActivatesThenAsksTheExtensionHostToRetry) {
    const auto first = request("$executeCommand", R"(["lazy.cmd",[1],true])", 7);
    ASSERT_EQ(first.size(), 2u); // the acknowledgement and the activation call
    const auto& activation = std::get<Request>(first[1]);
    EXPECT_EQ(activation.proxy_id, static_cast<std::uint8_t>(ProxyId::ExtHostExtensionService));
    EXPECT_EQ(activation.method, "$activateByEvent");
    EXPECT_EQ(json::parse(std::get<JsonArgs>(activation.args).json),
              json::parse(R"(["onCommand:lazy.cmd",0])"));

    ASSERT_TRUE(host_.send_rpc(EmptyReply{activation.req}).has_value());
    const auto reply = host_.drain_rpc();
    ASSERT_EQ(reply.size(), 1u);
    EXPECT_EQ(json::parse(std::get<ErrorReply>(reply[0]).json)["message"], "$executeCommand:retry");
}

TEST_F(CommandsServiceTest, ForwardsExtensionCommandsAndRepliesWhenTheExtensionHostAnswers) {
    request("$registerCommand", R"(["ext.cmd"])");

    const auto first = request("$executeCommand", R"(["ext.cmd",["x",2],true])", 9);
    ASSERT_EQ(first.size(), 2u);
    const auto& forwarded = std::get<Request>(first[1]);
    EXPECT_EQ(forwarded.proxy_id, static_cast<std::uint8_t>(ProxyId::ExtHostCommands));
    EXPECT_EQ(forwarded.method, "$executeContributedCommand");
    EXPECT_EQ(json::parse(std::get<JsonArgs>(forwarded.args).json),
              json::parse(R"(["ext.cmd","x",2])"));

    ASSERT_TRUE(host_.send_rpc(JsonReply{forwarded.req, "\"result\""}).has_value());
    const auto reply = host_.drain_rpc();
    ASSERT_EQ(reply.size(), 1u);
    EXPECT_EQ(reply[0], (RpcMessage{JsonReply{9, "\"result\""}}));
}

TEST_F(CommandsServiceTest, ExtensionCommandErrorsPropagateToTheCaller) {
    request("$registerCommand", R"(["ext.cmd"])");
    const auto first = request("$executeCommand", R"(["ext.cmd",[],false])", 4);
    ASSERT_TRUE(host_.send_rpc(ErrorReply{std::get<Request>(first[1]).req, R"({"message":"bad"})"})
                    .has_value());
    const auto reply = host_.drain_rpc();
    ASSERT_EQ(reply.size(), 1u);
    EXPECT_EQ(reply[0], (RpcMessage{ErrorReply{4, R"({"message":"bad"})"}}));
}

TEST_F(CommandsServiceTest, FiresActivationEventsForKnownCommands) {
    const auto out = request("$fireCommandActivationEvent", R"(["ext.cmd"])");
    ASSERT_EQ(out.size(), 3u); // acknowledgement, the activation call, then the empty reply
    EXPECT_EQ(std::get<Request>(out[1]).method, "$activateByEvent");
    EXPECT_EQ(out[2], RpcMessage{EmptyReply{1}});
}

TEST_F(CommandsServiceTest, ExecuteRunsBuiltinCommandsForTheEditor) {
    commands_.add_builtin("b.echo", [](const json& args) -> CallResult { return args.dump(); });
    std::optional<CallResult> result;
    commands_.execute("b.echo", R"(["a",1])", [&](CallResult r) { result = std::move(r); });
    ASSERT_TRUE(result.has_value() && result->has_value());
    EXPECT_EQ(**result, R"(["a",1])");
}

TEST_F(CommandsServiceTest, ExecuteRunsExtensionCommandsInTheExtensionHost) {
    request("$registerCommand", R"(["ext.cmd"])");
    std::optional<CallResult> result;
    commands_.execute("ext.cmd", R"([5])", [&](CallResult r) { result = std::move(r); });
    EXPECT_FALSE(result.has_value());

    const auto out = host_.drain_rpc();
    ASSERT_EQ(out.size(), 1u);
    const auto& call = std::get<Request>(out[0]);
    EXPECT_EQ(json::parse(std::get<JsonArgs>(call.args).json), json::parse(R"(["ext.cmd",5])"));
    ASSERT_TRUE(host_.send_rpc(JsonReply{call.req, "10"}).has_value());
    ASSERT_TRUE(result.has_value() && result->has_value());
    EXPECT_EQ(**result, "10");
}

TEST_F(CommandsServiceTest, ExecuteReportsUnknownCommandsAndBadArguments) {
    std::optional<CallResult> result;
    commands_.execute("no.such", "[]", [&](CallResult r) { result = std::move(r); });
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());

    result.reset();
    commands_.execute("no.such", "{", [&](CallResult r) { result = std::move(r); });
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST_F(CommandsServiceTest, MalformedExecuteCommandArgumentsFail) {
    const auto out = request("$executeCommand", R"(["only-id"])");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<ErrorReply>(out[1]));
}
