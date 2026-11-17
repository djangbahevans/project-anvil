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
#include <string>
#include <utility>

namespace anvil::exthost {

// The arguments of a request as a JSON array. Undefined arguments become null; an argument that
// carries binary buffers is not supported and makes the request fail.
std::expected<nlohmann::json, Error> parse_arguments(const Request& request);

// The serialized form of an error that the extension host rethrows with `message`.
std::string error_json(std::string_view message);

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
