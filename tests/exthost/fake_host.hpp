#pragma once

#include "anvil/exthost/session.hpp"

#include <gtest/gtest.h>

namespace anvil::exthost::test {

// Plays the extension host's side of the connection.
class FakeHost {
public:
    explicit FakeHost(Session& session) : session_(session) {}

    std::expected<void, anvil::Error> send_regular(Bytes payload) {
        return deliver(encode_frame({FrameType::regular, ++sent_id_, 0, std::move(payload)}));
    }

    std::expected<void, anvil::Error> send_rpc(const RpcMessage& message) {
        return send_regular(*encode_rpc_message(message));
    }

    std::expected<void, anvil::Error> deliver(const Bytes& wire) {
        return session_.receive(wire);
    }

    // Everything the session wrote since the last call, as frames.
    std::vector<Frame> drain() {
        decoder_.feed(session_.take_output());
        std::vector<Frame> frames;
        while (true) {
            auto next = decoder_.next();
            EXPECT_TRUE(next.has_value());
            if (!next || !*next) {
                break;
            }
            frames.push_back(std::move(**next));
        }
        return frames;
    }

    std::vector<RpcMessage> drain_rpc() {
        std::vector<RpcMessage> messages;
        for (auto& frame : drain()) {
            if (frame.type == FrameType::regular) {
                auto message = decode_rpc_message(frame.payload);
                EXPECT_TRUE(message.has_value());
                if (message) {
                    messages.push_back(std::move(*message));
                }
            }
        }
        return messages;
    }

    void complete_handshake() {
        ASSERT_TRUE(send_regular(Bytes{std::byte{2}}).has_value());
        ASSERT_TRUE(send_regular(Bytes{std::byte{1}}).has_value());
        drain();
    }

private:
    Session& session_;
    FrameDecoder decoder_;
    std::uint32_t sent_id_ = 0;
};

} // namespace anvil::exthost::test
