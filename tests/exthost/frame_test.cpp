#include "anvil/exthost/frame.hpp"

#include <gtest/gtest.h>

using anvil::Bytes;
using anvil::ErrorCode;
using anvil::exthost::encode_frame;
using anvil::exthost::Frame;
using anvil::exthost::FrameDecoder;
using anvil::exthost::FrameType;

namespace {

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes out;
    for (unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

} // namespace

TEST(EncodeFrame, WritesBigEndianHeaderThenPayload) {
    const Frame frame{FrameType::regular, 0x01020304, 0x0A0B0C0D, bytes({0xAA, 0xBB})};
    EXPECT_EQ(encode_frame(frame), bytes({0x01, 0x01, 0x02, 0x03, 0x04, 0x0A, 0x0B, 0x0C, 0x0D,
                                          0x00, 0x00, 0x00, 0x02, 0xAA, 0xBB}));
}

TEST(EncodeFrame, HeaderOnlyFrameHasZeroLength) {
    const Frame frame{FrameType::resume, 0, 0, {}};
    EXPECT_EQ(encode_frame(frame), bytes({0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
}

TEST(FrameDecoder, DecodesAFrameFedAtOnce) {
    FrameDecoder decoder;
    const Frame original{FrameType::regular, 7, 3, bytes({1, 2, 3})};
    decoder.feed(encode_frame(original));

    const auto result = decoder.next();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->type, FrameType::regular);
    EXPECT_EQ((*result)->id, 7u);
    EXPECT_EQ((*result)->ack, 3u);
    EXPECT_EQ((*result)->payload, bytes({1, 2, 3}));

    const auto drained = decoder.next();
    ASSERT_TRUE(drained.has_value());
    EXPECT_FALSE(drained->has_value());
}

TEST(FrameDecoder, ReassemblesAFrameFedOneByteAtATime) {
    FrameDecoder decoder;
    const Bytes wire = encode_frame({FrameType::regular, 1, 0, bytes({9, 8, 7, 6})});

    for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
        decoder.feed(std::span{wire}.subspan(i, 1));
        const auto partial = decoder.next();
        ASSERT_TRUE(partial.has_value());
        EXPECT_FALSE(partial->has_value()) << "frame completed early at byte " << i;
    }
    decoder.feed(std::span{wire}.subspan(wire.size() - 1, 1));
    const auto result = decoder.next();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->payload, bytes({9, 8, 7, 6}));
}

TEST(FrameDecoder, SplitsBackToBackFrames) {
    FrameDecoder decoder;
    Bytes wire = encode_frame({FrameType::regular, 1, 0, bytes({1})});
    const Bytes second = encode_frame({FrameType::ack, 0, 1, {}});
    wire.insert(wire.end(), second.begin(), second.end());
    decoder.feed(wire);

    const auto first = decoder.next();
    ASSERT_TRUE(first.has_value() && first->has_value());
    EXPECT_EQ((*first)->type, FrameType::regular);

    const auto next = decoder.next();
    ASSERT_TRUE(next.has_value() && next->has_value());
    EXPECT_EQ((*next)->type, FrameType::ack);
    EXPECT_EQ((*next)->ack, 1u);
}

TEST(FrameDecoder, KeepsDecodingAcrossBufferCompaction) {
    FrameDecoder decoder;
    for (std::uint32_t i = 1; i <= 100; ++i) {
        decoder.feed(encode_frame({FrameType::regular, i, 0, Bytes(i, std::byte{0x5A})}));
        const auto result = decoder.next();
        ASSERT_TRUE(result.has_value() && result->has_value());
        EXPECT_EQ((*result)->id, i);
        EXPECT_EQ((*result)->payload.size(), i);
    }
}

TEST(FrameDecoder, RejectsUnknownFrameType) {
    for (unsigned type : {0u, 4u, 10u, 255u}) {
        FrameDecoder decoder;
        decoder.feed(bytes({type, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
        const auto result = decoder.next();
        ASSERT_FALSE(result.has_value()) << "accepted type " << type;
        EXPECT_EQ(result.error().code, ErrorCode::protocol_error);
    }
}

TEST(FrameDecoder, RejectsPayloadAboveLimitWithoutWaitingForIt) {
    FrameDecoder decoder(16);
    decoder.feed(bytes({1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 17}));
    const auto result = decoder.next();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::protocol_error);
}

TEST(FrameDecoder, StaysFailedAfterAProtocolError) {
    FrameDecoder decoder;
    decoder.feed(bytes({0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    ASSERT_FALSE(decoder.next().has_value());
    decoder.feed(encode_frame({FrameType::regular, 1, 0, {}}));
    EXPECT_FALSE(decoder.next().has_value());
}
