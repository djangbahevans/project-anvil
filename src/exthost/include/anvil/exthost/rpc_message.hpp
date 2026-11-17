#pragma once

#include "anvil/core/bytes.hpp"
#include "anvil/core/error.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace anvil::exthost {

// Request arguments that are all JSON values: the JSON text of an array.
struct JsonArgs {
    std::string json;
    bool operator==(const JsonArgs&) const = default;
};

// One argument of a request that contains buffers or undefined values.
struct JsonArg {
    std::string json;
    bool operator==(const JsonArg&) const = default;
};
struct BufferArg {
    Bytes data;
    bool operator==(const BufferArg&) const = default;
};
struct ObjectWithBuffersArg {
    std::string json;
    std::vector<Bytes> buffers;
    bool operator==(const ObjectWithBuffersArg&) const = default;
};
struct UndefinedArg {
    bool operator==(const UndefinedArg&) const = default;
};
using MixedArg = std::variant<JsonArg, BufferArg, ObjectWithBuffersArg, UndefinedArg>;
using MixedArgs = std::vector<MixedArg>;

// A call to method `method` of the service identified by `proxy_id`. `cancellable` requests can
// later be aborted with a Cancel message carrying the same `req`.
struct Request {
    std::uint32_t req;
    std::uint8_t proxy_id;
    std::string method;
    std::variant<JsonArgs, MixedArgs> args;
    bool cancellable = false;
    bool operator==(const Request&) const = default;
};

struct Acknowledged {
    std::uint32_t req;
    bool operator==(const Acknowledged&) const = default;
};

struct Cancel {
    std::uint32_t req;
    bool operator==(const Cancel&) const = default;
};

struct EmptyReply {
    std::uint32_t req;
    bool operator==(const EmptyReply&) const = default;
};
struct BufferReply {
    std::uint32_t req;
    Bytes data;
    bool operator==(const BufferReply&) const = default;
};
struct JsonReply {
    std::uint32_t req;
    std::string json;
    bool operator==(const JsonReply&) const = default;
};
struct JsonWithBuffersReply {
    std::uint32_t req;
    std::string json;
    std::vector<Bytes> buffers;
    bool operator==(const JsonWithBuffersReply&) const = default;
};
// `json` is the serialized error, or empty when the handler failed without detail.
struct ErrorReply {
    std::uint32_t req;
    std::string json;
    bool operator==(const ErrorReply&) const = default;
};

using RpcMessage = std::variant<Request, Acknowledged, Cancel, EmptyReply, BufferReply, JsonReply,
                                JsonWithBuffersReply, ErrorReply>;

std::expected<Bytes, Error> encode_rpc_message(const RpcMessage& message);
std::expected<RpcMessage, Error> decode_rpc_message(std::span<const std::byte> payload);

} // namespace anvil::exthost
