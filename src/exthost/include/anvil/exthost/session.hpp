#pragma once

#include "anvil/core/bytes.hpp"
#include "anvil/core/error.hpp"
#include "anvil/exthost/frame.hpp"
#include "anvil/exthost/proxy_ids.hpp"
#include "anvil/exthost/rpc_message.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>

namespace anvil::exthost {

// Outcome of handling a request from the extension host.
struct Ok {};
struct OkJson {
    std::string json;
};
struct Failed {
    std::string error_json;
};
using HandlerResult = std::variant<Ok, OkJson, Failed>;

// Outcome of a call to the extension host: the reply's JSON text (std::nullopt for an undefined
// result), or the serialized error.
using CallResult = std::expected<std::optional<std::string>, std::string>;

enum class SessionState {
    awaiting_ready,
    awaiting_initialized,
    running,
    closed,
};

// The editor side of one extension host connection, independent of any I/O: bytes read from the
// socket go into receive(), bytes to write to the socket come out of take_output(). Handlers run
// synchronously inside receive() and call().
//
// After the extension host announces itself the session sends the init data, waits for the
// extension host to confirm, and then calls the ready handler. From then on it numbers and
// acknowledges frames, acknowledges and dispatches requests from the extension host, and matches
// replies to calls made with call().
class Session {
public:
    // Returns the reply, or std::nullopt to reply later with reply().
    using RequestHandler = std::function<std::optional<HandlerResult>(const Request&)>;
    using ReplyHandler = std::function<void(CallResult)>;
    using ReadyHandler = std::function<void()>;

    explicit Session(std::string init_data_json);

    // Requests without a handler are acknowledged and answered with an empty reply.
    void set_request_handler(RequestHandler handler);
    void set_ready_handler(ReadyHandler handler);

    // A protocol violation closes the session and is returned; later calls return the same state.
    std::expected<void, Error> receive(std::span<const std::byte> data);

    // Bytes queued for the socket since the last call.
    Bytes take_output();

    // Calls `method` of an extension host service with `args_json`, the JSON text of the argument
    // array. `on_reply` runs when the reply arrives, or immediately with an error if the session
    // is not running.
    void call(ProxyId target, std::string method, std::string args_json, ReplyHandler on_reply);

    // Sends the reply to a request whose handler returned std::nullopt.
    void reply(std::uint32_t req, HandlerResult result);

    // Asks the extension host to shut down.
    void terminate();

    SessionState state() const {
        return state_;
    }

private:
    std::expected<void, Error> handle_frame(Frame frame);
    std::expected<void, Error> handle_payload(const Bytes& payload);
    void handle_rpc(RpcMessage message);
    void send_regular(Bytes payload);
    void send_frame(Frame frame);
    void send_rpc(const RpcMessage& message);
    std::expected<void, Error> fail(Error error);

    std::string init_data_json_;
    SessionState state_ = SessionState::awaiting_ready;
    FrameDecoder decoder_;
    Bytes output_;
    std::uint32_t last_sent_id_ = 0;
    std::uint32_t last_received_id_ = 0;
    bool needs_ack_ = false;
    std::uint32_t next_request_ = 1;
    std::unordered_map<std::uint32_t, ReplyHandler> pending_calls_;
    RequestHandler request_handler_;
    ReadyHandler ready_handler_;
};

} // namespace anvil::exthost
