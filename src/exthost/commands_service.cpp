#include "anvil/exthost/commands_service.hpp"

#include <algorithm>

namespace anvil::exthost {
namespace {

using nlohmann::json;

HandlerResult to_handler_result(CallResult result) {
    if (!result) {
        return Failed{std::move(result.error())};
    }
    if (!*result) {
        return Ok{};
    }
    return OkJson{std::move(**result)};
}

} // namespace

CommandsService::CommandsService(Session& session, RequestRouter& router) : session_(session) {
    router.on(ProxyId::MainThreadCommands, "$registerCommand",
              [this](const Request&, const json& args) -> std::optional<HandlerResult> {
                  extension_commands_.insert(args.at(0).get<std::string>());
                  return Ok{};
              });
    router.on(ProxyId::MainThreadCommands, "$unregisterCommand",
              [this](const Request&, const json& args) -> std::optional<HandlerResult> {
                  extension_commands_.erase(args.at(0).get<std::string>());
                  return Ok{};
              });
    router.on(ProxyId::MainThreadCommands, "$fireCommandActivationEvent",
              [this](const Request&, const json& args) -> std::optional<HandlerResult> {
                  const json activation_args =
                      json::array({"onCommand:" + args.at(0).get<std::string>(), 0});
                  session_.call(ProxyId::ExtHostExtensionService, "$activateByEvent",
                                activation_args.dump(), [](CallResult) {});
                  return Ok{};
              });
    router.on(ProxyId::MainThreadCommands, "$getCommands",
              [this](const Request&, const json&) -> std::optional<HandlerResult> {
                  return OkJson{json(command_ids()).dump()};
              });
    router.on(ProxyId::MainThreadCommands, "$executeCommand",
              [this](const Request& request, const json& args) {
                  return execute_for_extension_host(request, args);
              });
}

void CommandsService::add_builtin(std::string id, BuiltinHandler handler) {
    builtins_[std::move(id)] = std::move(handler);
}

std::vector<std::string> CommandsService::command_ids() const {
    std::vector<std::string> ids(extension_commands_.begin(), extension_commands_.end());
    for (const auto& [id, handler] : builtins_) {
        if (!extension_commands_.contains(id)) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void CommandsService::run_extension_command(const std::string& id, const json& args,
                                            Session::ReplyHandler on_result) {
    json call_args = json::array({id});
    for (const auto& arg : args) {
        call_args.push_back(arg);
    }
    session_.call(ProxyId::ExtHostCommands, "$executeContributedCommand", call_args.dump(),
                  std::move(on_result));
}

void CommandsService::execute(const std::string& id, const std::string& args_json,
                              Session::ReplyHandler on_result) {
    const json args = json::parse(args_json, nullptr, false);
    if (!args.is_array()) {
        on_result(std::unexpected(error_json("command arguments must be a JSON array")));
        return;
    }
    if (extension_commands_.contains(id)) {
        run_extension_command(id, args, std::move(on_result));
        return;
    }
    if (const auto builtin = builtins_.find(id); builtin != builtins_.end()) {
        on_result(builtin->second(args));
        return;
    }
    on_result(std::unexpected(error_json("command '" + id + "' not found")));
}

std::optional<HandlerResult> CommandsService::execute_for_extension_host(const Request& request,
                                                                         const json& args) {
    if (!args.is_array() || args.size() < 3 || !args[0].is_string() || !args[1].is_array()) {
        return Failed{error_json("malformed $executeCommand arguments")};
    }
    const std::string id = args[0].get<std::string>();
    const json& command_args = args[1];
    const bool retry = args[2].is_boolean() && args[2].get<bool>();
    const std::uint32_t req = request.req;

    if (extension_commands_.contains(id)) {
        run_extension_command(id, command_args, [this, req](CallResult result) {
            session_.reply(req, to_handler_result(std::move(result)));
        });
        return std::nullopt;
    }
    if (const auto builtin = builtins_.find(id); builtin != builtins_.end()) {
        return to_handler_result(builtin->second(command_args));
    }
    if (retry && !command_args.empty()) {
        // The command may belong to an extension that is not active yet. Activating it and
        // asking the extension host to run the command again lets it execute locally, with
        // arguments that are not limited to JSON.
        const json activation_args = json::array({"onCommand:" + id, 0});
        session_.call(ProxyId::ExtHostExtensionService, "$activateByEvent", activation_args.dump(),
                      [this, req](CallResult) {
                          session_.reply(req, Failed{error_json("$executeCommand:retry")});
                      });
        return std::nullopt;
    }
    return Failed{error_json("command '" + id + "' not found")};
}

} // namespace anvil::exthost
