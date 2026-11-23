#pragma once

#include "anvil/core/error.hpp"
#include "anvil/exthost/proxy_ids.hpp"
#include "anvil/exthost/rpc_message.hpp"
#include "anvil/exthost/session.hpp"

#include <expected>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace anvil::exthost {

// The arguments of a request as a JSON array. Undefined arguments become null, and so does an
// argument that is a buffer; an object with buffers is its JSON as it was sent, in which each
// buffer is the placeholder `{"$$ref$$": n}` for the n-th of its buffers. The bytes are read with
// `buffer_argument` and `object_buffers`.
std::expected<nlohmann::json, Error> parse_arguments(const Request& request);

// The buffer that the argument at `index` of the request is, or nullptr when there is no such
// argument or it is not a buffer. The bytes belong to the request.
const Bytes* buffer_argument(const Request& request, std::size_t index);

// The buffers of the object with buffers at `index`, in the order that its `$$ref$$` placeholders
// number them; empty for any other argument.
std::span<const Bytes> object_buffers(const Request& request, std::size_t index);

// The serialized form of an error that the extension host rethrows with `message`.
std::string error_json(std::string_view message);
// The same with the error's `name`, which the extension host reads for the errors it tells apart,
// such as the codes of file system errors.
std::string error_json(std::string_view name, std::string_view message);

// Dispatches requests from the extension host to the handler registered for the service and
// method. Requests nothing handles are answered with an empty reply and remembered by name, so
// the services still to implement can be found by running real extensions.
class RequestRouter {
public:
    // Like Session::RequestHandler, with the arguments already parsed. Returns std::nullopt to
    // reply later with Session::reply.
    using Handler =
        std::function<std::optional<HandlerResult>(const Request&, const nlohmann::json& args)>;

    void on(ProxyId service, std::string method, Handler handler);

    std::optional<HandlerResult> dispatch(const Request& request);

    // "Service.method" names of requests that had no handler.
    const std::set<std::string>& unhandled() const {
        return unhandled_;
    }

private:
    std::map<std::pair<std::uint8_t, std::string>, Handler> handlers_;
    std::set<std::string> unhandled_;
};

} // namespace anvil::exthost
