#pragma once

#include "anvil/exthost/request_router.hpp"
#include "anvil/exthost/session.hpp"

#include <expected>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace anvil::exthost {

// The editor side of the extension host's command registry (VS Code's MainThreadCommands).
//
// Commands live in two places: built-in commands run in the editor, and commands that extensions
// register run in the extension host. Extensions invoke either kind through $executeCommand, and
// the editor invokes either kind through execute().
//
// The session and router must outlive the service.
class CommandsService {
public:
    // Runs a built-in command with the JSON array of arguments. The result is the JSON text of
    // the return value (std::nullopt for undefined) or the serialized error.
    using BuiltinHandler = std::function<CallResult(const nlohmann::json& args)>;

    CommandsService(Session& session, RequestRouter& router);

    void add_builtin(std::string id, BuiltinHandler handler);

    // Commands registered by extensions.
    const std::set<std::string>& extension_commands() const {
        return extension_commands_;
    }

    // Built-in and extension command ids.
    std::vector<std::string> command_ids() const;

    // Runs a command with `args_json`, the JSON text of the argument array, and reports the
    // outcome to `on_result`.
    void execute(const std::string& id, const std::string& args_json,
                 Session::ReplyHandler on_result);

private:
    std::optional<HandlerResult> execute_for_extension_host(const Request& request,
                                                            const nlohmann::json& args);
    void run_extension_command(const std::string& id, const nlohmann::json& args,
                               Session::ReplyHandler on_result);

    Session& session_;
    std::unordered_map<std::string, BuiltinHandler> builtins_;
    std::set<std::string> extension_commands_;
};

} // namespace anvil::exthost
