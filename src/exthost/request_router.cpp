#include "anvil/exthost/request_router.hpp"

namespace anvil::exthost {
namespace {

std::expected<nlohmann::json, Error> parse_json(const std::string& text) {
    auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        return std::unexpected(
            Error{ErrorCode::protocol_error, "request argument is not valid JSON"});
    }
    return parsed;
}

} // namespace

std::expected<nlohmann::json, Error> parse_arguments(const Request& request) {
    if (const auto* json_args = std::get_if<JsonArgs>(&request.args)) {
        return parse_json(json_args->json);
    }

    nlohmann::json result = nlohmann::json::array();
    for (const auto& arg : std::get<MixedArgs>(request.args)) {
        if (const auto* json_arg = std::get_if<JsonArg>(&arg)) {
            auto parsed = parse_json(json_arg->json);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            result.push_back(std::move(*parsed));
        } else if (const auto* object = std::get_if<ObjectWithBuffersArg>(&arg)) {
            auto parsed = parse_json(object->json);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            result.push_back(std::move(*parsed));
        } else {
            result.push_back(nullptr);
        }
    }
    return result;
}

const Bytes* buffer_argument(const Request& request, std::size_t index) {
    const auto* mixed = std::get_if<MixedArgs>(&request.args);
    if (!mixed || index >= mixed->size()) {
        return nullptr;
    }
    const auto* buffer = std::get_if<BufferArg>(&(*mixed)[index]);
    return buffer ? &buffer->data : nullptr;
}

std::span<const Bytes> object_buffers(const Request& request, std::size_t index) {
    const auto* mixed = std::get_if<MixedArgs>(&request.args);
    if (!mixed || index >= mixed->size()) {
        return {};
    }
    const auto* object = std::get_if<ObjectWithBuffersArg>(&(*mixed)[index]);
    return object ? std::span<const Bytes>(object->buffers) : std::span<const Bytes>();
}

std::string error_json(std::string_view message) {
    return error_json("Error", message);
}

std::string error_json(std::string_view name, std::string_view message) {
    return nlohmann::json{{"$isError", true}, {"name", name}, {"message", message}}.dump();
}

void RequestRouter::on(ProxyId service, std::string method, Handler handler) {
    handlers_[{static_cast<std::uint8_t>(service), std::move(method)}] = std::move(handler);
}

std::optional<HandlerResult> RequestRouter::dispatch(const Request& request) {
    const auto found = handlers_.find({request.proxy_id, request.method});
    if (found == handlers_.end()) {
        unhandled_.insert(std::string(proxy_name(request.proxy_id)) + "." + request.method);
        return Ok{};
    }
    const auto args = parse_arguments(request);
    if (!args) {
        return Failed{error_json(args.error().message)};
    }
    // Handlers read arguments with the JSON library's throwing accessors; input from the
    // extension host that has the wrong shape becomes an error reply instead of escaping.
    try {
        return found->second(request, *args);
    } catch (const nlohmann::json::exception& exception) {
        return Failed{error_json(std::string("malformed arguments: ") + exception.what())};
    }
}

} // namespace anvil::exthost
