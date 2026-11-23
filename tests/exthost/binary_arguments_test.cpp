#include "anvil/exthost/request_router.hpp"
#include "fake_host.hpp"

#include <gtest/gtest.h>

using namespace anvil::exthost;
using anvil::Bytes;
using anvil::exthost::test::FakeHost;
using nlohmann::json;

namespace {

Bytes bytes_of(std::initializer_list<int> values) {
    Bytes bytes;
    for (const int value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

// A service with handlers that read binary arguments and reply with buffers, on a session whose
// extension host is played by FakeHost.
class BinaryArgumentsTest : public ::testing::Test {
protected:
    BinaryArgumentsTest() : session_("{}"), host_(session_) {
        router_.on(ProxyId::MainThreadFileSystem, "$echoBuffer",
                   [](const Request& request, const json&) -> std::optional<HandlerResult> {
                       const Bytes* buffer = buffer_argument(request, 1);
                       if (!buffer) {
                           return Failed{error_json("the second argument is not a buffer")};
                       }
                       return OkBuffer{*buffer};
                   });
        router_.on(ProxyId::MainThreadFileSystem, "$referenced",
                   [](const Request& request, const json& args) -> std::optional<HandlerResult> {
                       const auto buffers = object_buffers(request, 0);
                       const std::size_t index =
                           args.at(0).at("wanted").at("$$ref$$").get<std::size_t>();
                       return OkBuffer{buffers[index]};
                   });
        router_.on(ProxyId::MainThreadFileSystem, "$describe",
                   [](const Request& request, const json& args) -> std::optional<HandlerResult> {
                       json result = {{"args", args}, {"buffers", json::array()}};
                       for (std::size_t i = 0; i < args.size(); ++i) {
                           if (const Bytes* buffer = buffer_argument(request, i)) {
                               result["buffers"].push_back({i, buffer->size()});
                           }
                       }
                       return OkJson{result.dump()};
                   });
        session_.set_request_handler(
            [this](const Request& request) { return router_.dispatch(request); });
        host_.complete_handshake();
    }

    Request request(std::string method, MixedArgs args, std::uint32_t req = 1) {
        return Request{req, static_cast<std::uint8_t>(ProxyId::MainThreadFileSystem),
                       std::move(method), std::move(args)};
    }

    // What the session sent for the request, without the acknowledgement.
    RpcMessage reply_to(const Request& sent) {
        EXPECT_TRUE(host_.send_rpc(sent).has_value());
        auto messages = host_.drain_rpc();
        EXPECT_EQ(messages.size(), 2U);
        if (messages.size() != 2) {
            return EmptyReply{0};
        }
        EXPECT_EQ(messages[0], RpcMessage{Acknowledged{sent.req}});
        return messages[1];
    }

    Session session_;
    FakeHost host_;
    RequestRouter router_;
};

} // namespace

TEST_F(BinaryArgumentsTest, ABufferArgumentReachesTheHandlerAndComesBackAsABufferReply) {
    const Bytes content = bytes_of({0, 1, 2, 0xFF, 0x80, '\r', '\n', 0});
    const RpcMessage reply =
        reply_to(request("$echoBuffer", {JsonArg{"{\"path\":\"/a\"}"}, BufferArg{content}}, 7));
    ASSERT_TRUE(std::holds_alternative<BufferReply>(reply));
    EXPECT_EQ(std::get<BufferReply>(reply).req, 7U);
    EXPECT_EQ(std::get<BufferReply>(reply).data, content);
}

TEST_F(BinaryArgumentsTest, AnEmptyBufferIsAnEmptyBufferReplyAndNotAnEmptyReply) {
    const RpcMessage reply = reply_to(request("$echoBuffer", {JsonArg{"1"}, BufferArg{{}}}));
    ASSERT_TRUE(std::holds_alternative<BufferReply>(reply));
    EXPECT_TRUE(std::get<BufferReply>(reply).data.empty());
}

TEST_F(BinaryArgumentsTest, AnObjectWithBuffersGivesItsBuffersByTheirPlaceholders) {
    const RpcMessage reply = reply_to(
        request("$referenced",
                {ObjectWithBuffersArg{R"({"unwanted":{"$$ref$$":0},"wanted":{"$$ref$$":2}})",
                                      {bytes_of({1}), bytes_of({2, 2}), bytes_of({3, 3, 3})}}}));
    ASSERT_TRUE(std::holds_alternative<BufferReply>(reply));
    EXPECT_EQ(std::get<BufferReply>(reply).data, bytes_of({3, 3, 3}));
}

TEST_F(BinaryArgumentsTest, MixedArgumentsWithUndefinedAndBuffersAllArrive) {
    const RpcMessage reply = reply_to(
        request("$describe", {JsonArg{"\"a\""}, UndefinedArg{}, BufferArg{bytes_of({9, 9, 9})},
                              JsonArg{"[1]"}, BufferArg{{}}}));
    ASSERT_TRUE(std::holds_alternative<JsonReply>(reply));
    EXPECT_EQ(json::parse(std::get<JsonReply>(reply).json),
              json::parse(R"({"args":["a",null,null,[1],null],"buffers":[[2,3],[4,0]]})"));
}

TEST_F(BinaryArgumentsTest, ARequestWithOnlyJsonArgumentsIsHandledAsBefore) {
    const RpcMessage reply =
        reply_to(Request{1, static_cast<std::uint8_t>(ProxyId::MainThreadFileSystem), "$describe",
                         JsonArgs{R"(["x",2])"}});
    ASSERT_TRUE(std::holds_alternative<JsonReply>(reply));
    EXPECT_EQ(json::parse(std::get<JsonReply>(reply).json),
              json::parse(R"({"args":["x",2],"buffers":[]})"));
}

TEST_F(BinaryArgumentsTest, AHandlerThatFindsNoBufferFailsTheRequest) {
    const RpcMessage reply = reply_to(request("$echoBuffer", {JsonArg{"1"}, JsonArg{"2"}}));
    ASSERT_TRUE(std::holds_alternative<ErrorReply>(reply));
    EXPECT_NE(std::get<ErrorReply>(reply).json.find("not a buffer"), std::string::npos);
}

TEST_F(BinaryArgumentsTest, ABufferOfSeveralMegabytesArrivesInPiecesAndComesBackWhole) {
    Bytes large(3 * 1024 * 1024 + 17);
    for (std::size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<std::byte>((i * 31 + i / 251) & 0xFF);
    }
    const Request sent = request("$echoBuffer", {JsonArg{"1"}, BufferArg{large}}, 3);
    const auto payload = encode_rpc_message(sent);
    ASSERT_TRUE(payload.has_value());
    // The first two frames of the host were the handshake.
    const Bytes wire = encode_frame({FrameType::regular, 3, 0, *payload});
    for (std::size_t offset = 0; offset < wire.size(); offset += 4093) {
        const std::size_t length = std::min<std::size_t>(4093, wire.size() - offset);
        ASSERT_TRUE(host_
                        .deliver(Bytes(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                                       wire.begin() + static_cast<std::ptrdiff_t>(offset + length)))
                        .has_value());
    }
    const auto messages = host_.drain_rpc();
    ASSERT_EQ(messages.size(), 2U);
    ASSERT_TRUE(std::holds_alternative<BufferReply>(messages[1]));
    EXPECT_EQ(std::get<BufferReply>(messages[1]).data, large);
}
