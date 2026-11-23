#include "anvil/exthost/session.hpp"
#include "fake_host.hpp"

#include <gtest/gtest.h>

using namespace anvil::exthost;
using anvil::Bytes;
using anvil::ErrorCode;
using anvil::to_bytes;
using anvil::exthost::test::FakeHost;

TEST(Session, AnswersReadyWithInitDataAndBecomesRunningOnInitialized) {
    Session session("{\"init\":true}");
    bool ready = false;
    session.set_ready_handler([&] { ready = true; });
    FakeHost host(session);

    ASSERT_TRUE(host.send_regular(Bytes{std::byte{2}}).has_value());
    EXPECT_EQ(session.state(), SessionState::awaiting_initialized);
    const auto frames = host.drain();
    ASSERT_EQ(frames.size(), 2u); // the init data and an acknowledgement
    EXPECT_EQ(frames[0].type, FrameType::regular);
    EXPECT_EQ(frames[0].id, 1u);
    EXPECT_EQ(anvil::as_text(frames[0].payload), "{\"init\":true}");
    EXPECT_EQ(frames[1].type, FrameType::ack);
    EXPECT_EQ(frames[1].ack, 1u);
    EXPECT_FALSE(ready);

    ASSERT_TRUE(host.send_regular(Bytes{std::byte{1}}).has_value());
    EXPECT_EQ(session.state(), SessionState::running);
    EXPECT_TRUE(ready);
}

TEST(Session, IgnoresFramesThatCarryNoPayload) {
    Session session("{}");
    FakeHost host(session);
    for (FrameType type :
         {FrameType::resume, FrameType::keep_alive, FrameType::pause, FrameType::ack}) {
        ASSERT_TRUE(host.deliver(encode_frame({type, 0, 0, {}})).has_value());
    }
    EXPECT_EQ(session.state(), SessionState::awaiting_ready);
    EXPECT_TRUE(session.take_output().empty());
}

TEST(Session, RejectsAnUnexpectedHandshakePayload) {
    Session session("{}");
    FakeHost host(session);
    const auto result = host.send_regular(Bytes{std::byte{9}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::protocol_error);
    EXPECT_EQ(session.state(), SessionState::closed);
    EXPECT_FALSE(host.deliver(encode_frame({FrameType::regular, 2, 0, {}})).has_value());
}

TEST(Session, DisconnectFrameClosesTheSession) {
    Session session("{}");
    FakeHost host(session);
    ASSERT_TRUE(host.deliver(encode_frame({FrameType::disconnect, 0, 0, {}})).has_value());
    EXPECT_EQ(session.state(), SessionState::closed);
}

TEST(Session, AcknowledgesRequestsAndRepliesWithTheHandlerResult) {
    Session session("{}");
    FakeHost host(session);
    host.complete_handshake();

    std::optional<Request> seen;
    session.set_request_handler([&](const Request& request) -> std::optional<HandlerResult> {
        seen = request;
        return OkJson{"{\"isFocused\":true}"};
    });

    const Request request{5, static_cast<std::uint8_t>(ProxyId::MainThreadWindow),
                          "$getInitialState", JsonArgs{"[]"}};
    ASSERT_TRUE(host.send_rpc(request).has_value());

    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(*seen, request);
    const auto out = host.drain_rpc();
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], RpcMessage{Acknowledged{5}});
    EXPECT_EQ(out[1], (RpcMessage{JsonReply{5, "{\"isFocused\":true}"}}));
}

TEST(Session, RequestsWithoutAHandlerGetAnEmptyReply) {
    Session session("{}");
    FakeHost host(session);
    host.complete_handshake();

    ASSERT_TRUE(host.send_rpc(Request{1, 10, "$registerCommand", JsonArgs{"[\"x\"]"}}).has_value());
    const auto out = host.drain_rpc();
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1], RpcMessage{EmptyReply{1}});
}

TEST(Session, DeferredRequestsAreRepliedToLater) {
    Session session("{}");
    FakeHost host(session);
    host.complete_handshake();
    session.set_request_handler([](const Request&) { return std::nullopt; });

    ASSERT_TRUE(host.send_rpc(Request{3, 10, "$slow", JsonArgs{"[]"}}).has_value());
    EXPECT_EQ(host.drain_rpc().size(), 1u); // only the acknowledgement
    session.reply(3, Failed{"{\"message\":\"no\"}"});
    const auto out = host.drain_rpc();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], (RpcMessage{ErrorReply{3, "{\"message\":\"no\"}"}}));
}

TEST(Session, CallsAreNumberedAndMatchedToTheirReplies) {
    Session session("{}");
    FakeHost host(session);
    host.complete_handshake();

    std::vector<std::string> results;
    auto record = [&](CallResult result) {
        results.push_back(result ? result->value_or("<undefined>") : "error:" + result.error());
    };
    session.call(ProxyId::ExtHostCommands, "$executeContributedCommand", "[\"a\"]", record);
    session.call(ProxyId::ExtHostCommands, "$executeContributedCommand", "[\"b\"]", record);
    session.call(ProxyId::ExtHostWorkspace, "$initializeWorkspace", "[null,true]", record);

    const auto out = host.drain_rpc();
    ASSERT_EQ(out.size(), 3u);
    const auto& first = std::get<Request>(out[0]);
    EXPECT_EQ(first.req, 1u);
    EXPECT_EQ(first.proxy_id, static_cast<std::uint8_t>(ProxyId::ExtHostCommands));
    EXPECT_EQ(first.method, "$executeContributedCommand");
    EXPECT_EQ(std::get<JsonArgs>(first.args).json, "[\"a\"]");
    EXPECT_EQ(std::get<Request>(out[2]).req, 3u);

    // replies may arrive in any order
    ASSERT_TRUE(host.send_rpc(JsonReply{2, "\"second\""}).has_value());
    ASSERT_TRUE(host.send_rpc(EmptyReply{3}).has_value());
    ASSERT_TRUE(host.send_rpc(ErrorReply{1, "{\"message\":\"boom\"}"}).has_value());
    EXPECT_EQ(results, (std::vector<std::string>{"\"second\"", "<undefined>",
                                                 "error:{\"message\":\"boom\"}"}));
}

TEST(Session, PendingCallsCanBeFailedAndTheirLateRepliesAreDropped) {
    Session session("{}");
    FakeHost host(session);
    host.complete_handshake();

    std::vector<std::string> results;
    auto record = [&](CallResult result) {
        results.push_back(result ? result->value_or("<undefined>") : "error:" + result.error());
    };
    session.call(ProxyId::ExtHostCommands, "$a", "[]", record);
    session.call(ProxyId::ExtHostCommands, "$b", "[]", record);
    ASSERT_TRUE(host.send_rpc(EmptyReply{1}).has_value());
    ASSERT_EQ(results, (std::vector<std::string>{"<undefined>"}));

    session.fail_pending_calls("gone");
    EXPECT_EQ(results, (std::vector<std::string>{"<undefined>", "error:gone"}));

    ASSERT_TRUE(host.send_rpc(JsonReply{2, "\"late\""}).has_value());
    EXPECT_EQ(results.size(), 2u);
}

TEST(Session, CallBeforeTheSessionRunsFailsImmediately) {
    Session session("{}");
    std::optional<CallResult> result;
    session.call(ProxyId::ExtHostCommands, "$x", "[]",
                 [&](CallResult r) { result = std::move(r); });
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
    EXPECT_TRUE(session.take_output().empty());
}

TEST(Session, ReplyToAnUnknownRequestIsIgnored) {
    Session session("{}");
    FakeHost host(session);
    host.complete_handshake();
    EXPECT_TRUE(host.send_rpc(JsonReply{99, "1"}).has_value());
    EXPECT_EQ(session.state(), SessionState::running);
}

TEST(Session, ARepliesHandlerMayMakeFurtherCalls) {
    Session session("{}");
    FakeHost host(session);
    host.complete_handshake();

    std::string final_result;
    session.call(ProxyId::ExtHostWorkspace, "$first", "[]", [&](CallResult) {
        session.call(ProxyId::ExtHostCommands, "$second", "[]",
                     [&](CallResult r) { final_result = **r; });
    });
    host.drain();
    ASSERT_TRUE(host.send_rpc(EmptyReply{1}).has_value());
    const auto out = host.drain_rpc();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(std::get<Request>(out[0]).method, "$second");
    ASSERT_TRUE(host.send_rpc(JsonReply{2, "\"done\""}).has_value());
    EXPECT_EQ(final_result, "\"done\"");
}

TEST(Session, OutgoingFramesAreNumberedAndCarryTheLatestAcknowledgement) {
    Session session("{}");
    FakeHost host(session);
    host.complete_handshake(); // the host has sent frames 1 and 2

    session.call(ProxyId::ExtHostCommands, "$a", "[]", [](CallResult) {});
    session.call(ProxyId::ExtHostCommands, "$b", "[]", [](CallResult) {});
    const auto frames = host.drain();
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].id, 2u); // frame 1 was the init data
    EXPECT_EQ(frames[1].id, 3u);
    EXPECT_EQ(frames[0].ack, 2u);
}

TEST(Session, TerminateSendsTheTerminateMarker) {
    Session session("{}");
    FakeHost host(session);
    host.complete_handshake();
    session.terminate();
    const auto frames = host.drain();
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].payload, Bytes{std::byte{3}});
}

TEST(Session, MalformedRpcPayloadClosesTheSession) {
    Session session("{}");
    FakeHost host(session);
    host.complete_handshake();
    const auto result = host.send_regular(Bytes{std::byte{0xEE}, std::byte{0}, std::byte{0}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(session.state(), SessionState::closed);
}
