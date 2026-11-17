#include "anvil/exthost/rpc_message.hpp"

#include <gtest/gtest.h>

using namespace anvil::exthost;
using anvil::Bytes;
using anvil::ErrorCode;
using anvil::to_bytes;

namespace {

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes out;
    for (unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

void expect_round_trip(const RpcMessage& message) {
    const auto encoded = encode_rpc_message(message);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    const auto decoded = decode_rpc_message(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(*decoded, message);
}

} // namespace

TEST(RpcMessage, EncodesJsonRequestToDocumentedLayout) {
    const Request request{7, 3, "$a", JsonArgs{"[1]"}};
    const auto encoded = encode_rpc_message(request);
    ASSERT_TRUE(encoded.has_value());
    // type, req (u32), proxy id, method length + bytes, args length (u32) + bytes
    EXPECT_EQ(*encoded, bytes({0x01, 0, 0, 0, 7, 3, 2, '$', 'a', 0, 0, 0, 3, '[', '1', ']'}));
}

TEST(RpcMessage, EncodesAcknowledgedAsFiveByteHeader) {
    const auto encoded = encode_rpc_message(Acknowledged{258});
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(*encoded, bytes({0x05, 0, 0, 1, 2}));
}

TEST(RpcMessage, RoundTripsEveryMessageKind) {
    expect_round_trip(Request{1, 12, "$registerCommand", JsonArgs{"[\"id\"]"}});
    expect_round_trip(Request{2, 12, "$x", JsonArgs{"[]"}, true});
    expect_round_trip(
        Request{3, 200, "$mixed",
                MixedArgs{JsonArg{"\"text\""}, BufferArg{bytes({1, 2, 3})}, UndefinedArg{},
                          ObjectWithBuffersArg{"{\"a\":1}", {bytes({9}), bytes({})}}}});
    expect_round_trip(Request{4, 1, "$m", MixedArgs{UndefinedArg{}}, true});
    expect_round_trip(Request{5, 1, "$empty", MixedArgs{}});
    expect_round_trip(Acknowledged{6});
    expect_round_trip(Cancel{7});
    expect_round_trip(EmptyReply{8});
    expect_round_trip(BufferReply{9, bytes({0xDE, 0xAD})});
    expect_round_trip(JsonReply{10, "{\"ok\":true}"});
    expect_round_trip(JsonWithBuffersReply{11, "[0,1]", {bytes({1}), bytes({2, 3})}});
    expect_round_trip(ErrorReply{12, "{\"message\":\"boom\"}"});
    expect_round_trip(ErrorReply{13, ""});
}

TEST(RpcMessage, PreservesUtf8InMethodAndJson) {
    expect_round_trip(Request{1, 1, "$m\xC3\xA9", JsonArgs{"[\"\xF0\x9F\x98\x80\"]"}});
}

TEST(RpcMessage, RejectsMessageShorterThanHeader) {
    const auto result = decode_rpc_message(bytes({0x05, 0, 0}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::protocol_error);
}

TEST(RpcMessage, RejectsUnknownType) {
    for (unsigned type : {0u, 13u, 255u}) {
        const auto result = decode_rpc_message(bytes({type, 0, 0, 0, 1}));
        ASSERT_FALSE(result.has_value()) << "accepted type " << type;
    }
}

TEST(RpcMessage, RejectsTruncatedBodies) {
    const auto encoded = encode_rpc_message(Request{1, 2, "$method", JsonArgs{"[1,2,3]"}});
    ASSERT_TRUE(encoded.has_value());
    for (std::size_t length = 5; length < encoded->size(); ++length) {
        const auto result = decode_rpc_message(std::span{*encoded}.first(length));
        EXPECT_FALSE(result.has_value()) << "accepted truncation to " << length << " bytes";
    }
}

TEST(RpcMessage, RejectsTrailingBytes) {
    auto encoded = encode_rpc_message(Acknowledged{1});
    ASSERT_TRUE(encoded.has_value());
    encoded->push_back(std::byte{0});
    EXPECT_FALSE(decode_rpc_message(*encoded).has_value());
}

TEST(RpcMessage, RejectsHugeLengthPrefixesWithoutAllocating) {
    // reply JSON whose declared length is 4 GiB - 1 but which carries no bytes
    const auto result = decode_rpc_message(bytes({0x09, 0, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF}));
    EXPECT_FALSE(result.has_value());

    // JSON-with-buffers reply declaring 4 billion buffers
    const auto many =
        decode_rpc_message(bytes({0x0A, 0, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0}));
    EXPECT_FALSE(many.has_value());
}

TEST(RpcMessage, RejectsUnknownMixedArgumentType) {
    const auto result = decode_rpc_message(bytes({0x03, 0, 0, 0, 1, 1, 1, '$', 1, 99}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::protocol_error);
}

TEST(RpcMessage, EncodeRejectsMethodNamesAbove255Bytes) {
    const Request request{1, 1, std::string(256, 'x'), JsonArgs{"[]"}};
    const auto encoded = encode_rpc_message(request);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, ErrorCode::protocol_error);
}
