#include "anvil/exthost/rpc_message.hpp"

#include "wire.hpp"

#include <format>
#include <limits>
#include <optional>

namespace anvil::exthost {
namespace {

enum class WireType : std::uint8_t {
    request_json = 1,
    request_json_cancellable = 2,
    request_mixed = 3,
    request_mixed_cancellable = 4,
    acknowledged = 5,
    cancel = 6,
    reply_empty = 7,
    reply_buffer = 8,
    reply_json = 9,
    reply_json_with_buffers = 10,
    reply_error = 11,
    reply_error_empty = 12,
};

enum class MixedArgType : std::uint8_t {
    json = 1,
    buffer = 2,
    object_with_buffers = 3,
    undefined = 4,
};

template <class... Ts> struct Overloaded : Ts... {
    using Ts::operator()...;
};

Error protocol_error(std::string message) {
    return Error{ErrorCode::protocol_error, std::move(message)};
}

// --- decoding ---

std::string read_short_string(wire::Reader& reader) {
    return std::string(as_text(reader.take(reader.u8())));
}

std::string read_long_string(wire::Reader& reader) {
    return std::string(as_text(reader.take(reader.u32())));
}

Bytes read_buffer(wire::Reader& reader) {
    const auto bytes = reader.take(reader.u32());
    return Bytes(bytes.begin(), bytes.end());
}

std::vector<Bytes> read_buffers(wire::Reader& reader, std::uint32_t count) {
    std::vector<Bytes> buffers;
    for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
        buffers.push_back(read_buffer(reader));
    }
    return buffers;
}

MixedArgs read_mixed_args(wire::Reader& reader, std::string& error) {
    MixedArgs args;
    const std::uint8_t count = reader.u8();
    for (std::uint8_t i = 0; i < count && reader.ok(); ++i) {
        switch (static_cast<MixedArgType>(reader.u8())) {
        case MixedArgType::json:
            args.emplace_back(JsonArg{read_long_string(reader)});
            break;
        case MixedArgType::buffer:
            args.emplace_back(BufferArg{read_buffer(reader)});
            break;
        case MixedArgType::object_with_buffers: {
            const std::uint32_t buffer_count = reader.u32();
            std::string json = read_long_string(reader);
            args.emplace_back(
                ObjectWithBuffersArg{std::move(json), read_buffers(reader, buffer_count)});
            break;
        }
        case MixedArgType::undefined:
            args.emplace_back(UndefinedArg{});
            break;
        default:
            error = "unknown argument type in request";
            return args;
        }
    }
    return args;
}

// --- encoding ---

class Encoder {
public:
    Encoder(WireType type, std::uint32_t req) {
        writer_.u8(static_cast<std::uint8_t>(type));
        writer_.u32(req);
    }

    void u8(std::uint8_t value) {
        writer_.u8(value);
    }
    void u32(std::uint32_t value) {
        writer_.u32(value);
    }

    void short_string(const std::string& text) {
        if (text.size() > std::numeric_limits<std::uint8_t>::max()) {
            fail(std::format("string of {} bytes does not fit a short string", text.size()));
            return;
        }
        writer_.u8(static_cast<std::uint8_t>(text.size()));
        writer_.raw(std::as_bytes(std::span{text}));
    }

    void long_string(const std::string& text) {
        if (!fits_u32(text.size())) {
            return;
        }
        writer_.u32(static_cast<std::uint32_t>(text.size()));
        writer_.raw(std::as_bytes(std::span{text}));
    }

    void buffer(const Bytes& data) {
        if (!fits_u32(data.size())) {
            return;
        }
        writer_.u32(static_cast<std::uint32_t>(data.size()));
        writer_.raw(data);
    }

    void buffer_count(const std::vector<Bytes>& list) {
        if (!fits_u32(list.size())) {
            return;
        }
        writer_.u32(static_cast<std::uint32_t>(list.size()));
    }

    void fail(std::string message) {
        if (!error_) {
            error_ = protocol_error(std::move(message));
        }
    }

    std::expected<Bytes, Error> finish() {
        if (error_) {
            return std::unexpected(*error_);
        }
        return writer_.take();
    }

private:
    bool fits_u32(std::size_t size) {
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            fail(std::format("length {} does not fit in 32 bits", size));
            return false;
        }
        return true;
    }

    wire::Writer writer_;
    std::optional<Error> error_;
};

void write_mixed_args(Encoder& encoder, const MixedArgs& args) {
    if (args.size() > std::numeric_limits<std::uint8_t>::max()) {
        encoder.fail(std::format("{} arguments exceed the 255 argument limit", args.size()));
        return;
    }
    encoder.u8(static_cast<std::uint8_t>(args.size()));
    for (const auto& arg : args) {
        std::visit(Overloaded{
                       [&](const JsonArg& a) {
                           encoder.u8(static_cast<std::uint8_t>(MixedArgType::json));
                           encoder.long_string(a.json);
                       },
                       [&](const BufferArg& a) {
                           encoder.u8(static_cast<std::uint8_t>(MixedArgType::buffer));
                           encoder.buffer(a.data);
                       },
                       [&](const ObjectWithBuffersArg& a) {
                           encoder.u8(static_cast<std::uint8_t>(MixedArgType::object_with_buffers));
                           encoder.buffer_count(a.buffers);
                           encoder.long_string(a.json);
                           for (const auto& b : a.buffers) {
                               encoder.buffer(b);
                           }
                       },
                       [&](const UndefinedArg&) {
                           encoder.u8(static_cast<std::uint8_t>(MixedArgType::undefined));
                       },
                   },
                   arg);
    }
}

std::expected<Bytes, Error> encode_request(const Request& request) {
    const bool is_json = std::holds_alternative<JsonArgs>(request.args);
    WireType type =
        request.cancellable ? WireType::request_json_cancellable : WireType::request_json;
    if (!is_json) {
        type = request.cancellable ? WireType::request_mixed_cancellable : WireType::request_mixed;
    }
    Encoder encoder(type, request.req);
    encoder.u8(request.proxy_id);
    encoder.short_string(request.method);
    if (is_json) {
        encoder.long_string(std::get<JsonArgs>(request.args).json);
    } else {
        write_mixed_args(encoder, std::get<MixedArgs>(request.args));
    }
    return encoder.finish();
}

} // namespace

std::expected<Bytes, Error> encode_rpc_message(const RpcMessage& message) {
    return std::visit(
        Overloaded{
            [](const Request& m) { return encode_request(m); },
            [](const Acknowledged& m) { return Encoder(WireType::acknowledged, m.req).finish(); },
            [](const Cancel& m) { return Encoder(WireType::cancel, m.req).finish(); },
            [](const EmptyReply& m) { return Encoder(WireType::reply_empty, m.req).finish(); },
            [](const BufferReply& m) {
                Encoder encoder(WireType::reply_buffer, m.req);
                encoder.buffer(m.data);
                return encoder.finish();
            },
            [](const JsonReply& m) {
                Encoder encoder(WireType::reply_json, m.req);
                encoder.long_string(m.json);
                return encoder.finish();
            },
            [](const JsonWithBuffersReply& m) {
                Encoder encoder(WireType::reply_json_with_buffers, m.req);
                encoder.buffer_count(m.buffers);
                encoder.long_string(m.json);
                for (const auto& b : m.buffers) {
                    encoder.buffer(b);
                }
                return encoder.finish();
            },
            [](const ErrorReply& m) {
                if (m.json.empty()) {
                    return Encoder(WireType::reply_error_empty, m.req).finish();
                }
                Encoder encoder(WireType::reply_error, m.req);
                encoder.long_string(m.json);
                return encoder.finish();
            },
        },
        message);
}

std::expected<RpcMessage, Error> decode_rpc_message(std::span<const std::byte> payload) {
    wire::Reader reader(payload);
    const std::uint8_t type = reader.u8();
    const std::uint32_t req = reader.u32();
    if (!reader.ok()) {
        return std::unexpected(protocol_error("RPC message shorter than its 5-byte header"));
    }

    std::string error;
    RpcMessage message = Acknowledged{req};

    switch (static_cast<WireType>(type)) {
    case WireType::request_json:
    case WireType::request_json_cancellable: {
        const std::uint8_t proxy_id = reader.u8();
        std::string method = read_short_string(reader);
        std::string json = read_long_string(reader);
        message = Request{req, proxy_id, std::move(method), JsonArgs{std::move(json)},
                          static_cast<WireType>(type) == WireType::request_json_cancellable};
        break;
    }
    case WireType::request_mixed:
    case WireType::request_mixed_cancellable: {
        const std::uint8_t proxy_id = reader.u8();
        std::string method = read_short_string(reader);
        MixedArgs args = read_mixed_args(reader, error);
        message = Request{req, proxy_id, std::move(method), std::move(args),
                          static_cast<WireType>(type) == WireType::request_mixed_cancellable};
        break;
    }
    case WireType::acknowledged:
        message = Acknowledged{req};
        break;
    case WireType::cancel:
        message = Cancel{req};
        break;
    case WireType::reply_empty:
        message = EmptyReply{req};
        break;
    case WireType::reply_buffer:
        message = BufferReply{req, read_buffer(reader)};
        break;
    case WireType::reply_json:
        message = JsonReply{req, read_long_string(reader)};
        break;
    case WireType::reply_json_with_buffers: {
        const std::uint32_t buffer_count = reader.u32();
        std::string json = read_long_string(reader);
        message = JsonWithBuffersReply{req, std::move(json), read_buffers(reader, buffer_count)};
        break;
    }
    case WireType::reply_error:
        message = ErrorReply{req, read_long_string(reader)};
        break;
    case WireType::reply_error_empty:
        message = ErrorReply{req, {}};
        break;
    default:
        return std::unexpected(protocol_error(std::format("unknown RPC message type {}", type)));
    }

    if (!error.empty()) {
        return std::unexpected(protocol_error(std::move(error)));
    }
    if (!reader.ok()) {
        return std::unexpected(
            protocol_error(std::format("truncated RPC message of type {}", type)));
    }
    if (!reader.at_end()) {
        return std::unexpected(
            protocol_error(std::format("trailing bytes after RPC message of type {}", type)));
    }
    return message;
}

} // namespace anvil::exthost
