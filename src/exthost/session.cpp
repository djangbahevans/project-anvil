#include "anvil/exthost/session.hpp"

#include <format>
#include <utility>

namespace anvil::exthost {
namespace {

constexpr std::byte ready_marker{2};
constexpr std::byte initialized_marker{1};
constexpr std::byte terminate_marker{3};

template <class... Ts> struct Overloaded : Ts... {
    using Ts::operator()...;
};

bool is_marker(const Bytes& payload, std::byte marker) {
    return payload.size() == 1 && payload[0] == marker;
}

Error protocol_error(std::string message) {
    return Error{ErrorCode::protocol_error, std::move(message)};
}

RpcMessage to_reply_message(std::uint32_t req, HandlerResult result) {
    return std::visit(
        Overloaded{
            [&](Ok) -> RpcMessage { return EmptyReply{req}; },
            [&](OkJson& r) -> RpcMessage { return JsonReply{req, std::move(r.json)}; },
            [&](OkBuffer& r) -> RpcMessage { return BufferReply{req, std::move(r.data)}; },
            [&](Failed& r) -> RpcMessage { return ErrorReply{req, std::move(r.error_json)}; },
        },
        result);
}

} // namespace

Session::Session(std::string init_data_json) : init_data_json_(std::move(init_data_json)) {}

void Session::set_request_handler(RequestHandler handler) {
    request_handler_ = std::move(handler);
}

void Session::set_ready_handler(ReadyHandler handler) {
    ready_handler_ = std::move(handler);
}

std::expected<void, Error> Session::fail(Error error) {
    state_ = SessionState::closed;
    return std::unexpected(std::move(error));
}

std::expected<void, Error> Session::receive(std::span<const std::byte> data) {
    if (state_ == SessionState::closed) {
        return std::unexpected(protocol_error("the session is closed"));
    }
    decoder_.feed(data);
    while (true) {
        auto next = decoder_.next();
        if (!next) {
            return fail(std::move(next.error()));
        }
        if (!*next) {
            break;
        }
        if (auto handled = handle_frame(std::move(**next)); !handled) {
            return handled;
        }
        if (state_ == SessionState::closed) {
            return {};
        }
    }
    if (needs_ack_) {
        send_frame(Frame{FrameType::ack, 0, last_received_id_, {}});
        needs_ack_ = false;
    }
    return {};
}

std::expected<void, Error> Session::handle_frame(Frame frame) {
    switch (frame.type) {
    case FrameType::regular:
        last_received_id_ = frame.id;
        needs_ack_ = true;
        return handle_payload(frame.payload);
    case FrameType::disconnect:
        state_ = SessionState::closed;
        return {};
    case FrameType::control:
    case FrameType::ack:
    case FrameType::replay_request:
    case FrameType::pause:
    case FrameType::resume:
    case FrameType::keep_alive:
        return {};
    }
    return {};
}

std::expected<void, Error> Session::handle_payload(const Bytes& payload) {
    switch (state_) {
    case SessionState::awaiting_ready:
        if (!is_marker(payload, ready_marker)) {
            return fail(protocol_error("expected the Ready message from the extension host"));
        }
        send_regular(to_bytes(init_data_json_));
        state_ = SessionState::awaiting_initialized;
        return {};
    case SessionState::awaiting_initialized:
        if (!is_marker(payload, initialized_marker)) {
            return fail(protocol_error("expected the Initialized message from the extension host"));
        }
        state_ = SessionState::running;
        if (ready_handler_) {
            ready_handler_();
        }
        return {};
    case SessionState::running: {
        auto message = decode_rpc_message(payload);
        if (!message) {
            return fail(std::move(message.error()));
        }
        handle_rpc(std::move(*message));
        return {};
    }
    case SessionState::closed:
        return {};
    }
    return {};
}

void Session::handle_rpc(RpcMessage message) {
    std::visit(
        Overloaded{
            [&](Request& request) {
                send_rpc(Acknowledged{request.req});
                std::optional<HandlerResult> result = request_handler_
                                                          ? request_handler_(request)
                                                          : std::optional<HandlerResult>(Ok{});
                if (result) {
                    send_rpc(to_reply_message(request.req, std::move(*result)));
                }
            },
            [&](Acknowledged&) {},
            [&](Cancel&) {},
            [&](auto& reply) {
                auto found = pending_calls_.find(reply.req);
                if (found == pending_calls_.end()) {
                    return;
                }
                ReplyHandler on_reply = std::move(found->second);
                pending_calls_.erase(found);
                using T = std::decay_t<decltype(reply)>;
                if constexpr (std::is_same_v<T, EmptyReply>) {
                    on_reply(std::optional<std::string>{});
                } else if constexpr (std::is_same_v<T, JsonReply>) {
                    on_reply(std::optional<std::string>{std::move(reply.json)});
                } else if constexpr (std::is_same_v<T, JsonWithBuffersReply>) {
                    on_reply(std::optional<std::string>{std::move(reply.json)});
                } else if constexpr (std::is_same_v<T, BufferReply>) {
                    on_reply(std::unexpected(std::string("buffer replies are not supported")));
                } else {
                    on_reply(std::unexpected(std::move(reply.json)));
                }
            },
        },
        message);
}

Bytes Session::take_output() {
    return std::exchange(output_, {});
}

void Session::call(ProxyId target, std::string method, std::string args_json,
                   ReplyHandler on_reply) {
    if (state_ != SessionState::running) {
        on_reply(std::unexpected(std::string("the extension host session is not running")));
        return;
    }
    const std::uint32_t req = next_request_++;
    auto encoded = encode_rpc_message(Request{req, static_cast<std::uint8_t>(target),
                                              std::move(method), JsonArgs{std::move(args_json)}});
    if (!encoded) {
        on_reply(std::unexpected(encoded.error().message));
        return;
    }
    pending_calls_.emplace(req, std::move(on_reply));
    send_regular(std::move(*encoded));
}

void Session::reply(std::uint32_t req, HandlerResult result) {
    if (state_ == SessionState::running) {
        send_rpc(to_reply_message(req, std::move(result)));
    }
}

void Session::terminate() {
    if (state_ == SessionState::running) {
        send_regular(Bytes{terminate_marker});
    }
}

void Session::send_rpc(const RpcMessage& message) {
    if (auto encoded = encode_rpc_message(message)) {
        send_regular(std::move(*encoded));
    }
}

void Session::send_regular(Bytes payload) {
    send_frame(Frame{FrameType::regular, ++last_sent_id_, last_received_id_, std::move(payload)});
}

void Session::send_frame(Frame frame) {
    const Bytes encoded = encode_frame(frame);
    output_.insert(output_.end(), encoded.begin(), encoded.end());
}

} // namespace anvil::exthost
