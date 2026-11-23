#include "anvil/exthost/extension_host.hpp"

#include "anvil/exthost/init_data.hpp"
#include "anvil/exthost/session_logs.hpp"

namespace anvil::exthost {
namespace {

using nlohmann::json;

// Configuration with no settings; a workbench with a settings store replaces it.
std::string initial_configuration_json() {
    const json model = {
        {"contents", json::object()}, {"keys", json::array()}, {"overrides", json::array()}};
    return json::array({{{"defaults", model},
                         {"policy", model},
                         {"application", model},
                         {"userLocal", model},
                         {"userRemote", model},
                         {"workspace", model},
                         {"folders", json::array()},
                         {"configurationScopes", json::array()}}})
        .dump();
}

// Answers for the calls the extension host makes while starting; it dereferences these results.
void register_startup_services(RequestRouter& router) {
    router.on(ProxyId::MainThreadWindow, "$getInitialState",
              [](const Request&, const json&) -> std::optional<HandlerResult> {
                  return OkJson{R"({"isFocused":true,"isActive":true})"};
              });
    // Language model tools; none are offered.
    router.on(
        ProxyId::MainThreadChatSkills, "$getTools",
        [](const Request&, const json&) -> std::optional<HandlerResult> { return OkJson{"[]"}; });
}

} // namespace

ExtensionHost::ExtensionHost(std::unique_ptr<HostProcess> host,
                             std::shared_ptr<core::Logger> logger,
                             std::optional<core::Workspace> workspace)
    : logger_(logger ? std::move(logger) : std::make_shared<core::NullLogger>()),
      host_(std::move(host)), commands_(host_->session(), router_),
      documents_(host_->session(), router_), logging_(logger_, router_), file_system_(router_),
      workspace_(std::move(workspace)) {
    register_startup_services(router_);
    host_->session().set_request_handler(
        [this](const Request& request) { return router_.dispatch(request); });
    host_->session().set_ready_handler([this] {
        startup_steps_ = {
            {ProxyId::ExtHostConfiguration, "$initializeConfiguration",
             initial_configuration_json()},
            {ProxyId::ExtHostWorkspace, "$initializeWorkspace",
             workspace_initialization_args(workspace_)},
            // Extensions that activate at startup must already see the open documents.
            {ProxyId::ExtHostDocumentsAndEditors, "$acceptDocumentsAndEditorsDelta",
             documents_.initial_delta_args()},
            {ProxyId::ExtHostExtensionService, "$activateByEvent", R"(["*",0])"},
        };
        run_step(0);
    });
}

std::expected<std::unique_ptr<ExtensionHost>, Error>
ExtensionHost::start(const ExtensionHostOptions& options) {
    if (options.storage_dir.empty()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "the extension host needs a storage directory"});
    }
    // Each session logs in a directory of its own, so that a session shows only its own lines and
    // old logs do not pile up.
    auto logs = create_session_logs(options.storage_dir / "logs");
    if (!logs) {
        return std::unexpected(logs.error());
    }
    auto init_data = build_init_data(InitDataOptions{
        .app_root = options.vscode_dir,
        .extension_dirs = options.extension_dirs,
        .storage_dir = options.storage_dir,
        .logs_dir = *logs,
        .workspace = options.workspace,
    });
    if (!init_data) {
        return std::unexpected(init_data.error());
    }
    // The host runs in its storage directory, which the editor has made sure exists, instead of in
    // whatever directory the editor was started from.
    auto host = HostProcess::start(HostOptions{options.node_executable, options.vscode_dir,
                                               std::move(*init_data), options.connect_timeout,
                                               options.storage_dir});
    if (!host) {
        return std::unexpected(host.error());
    }
    return std::unique_ptr<ExtensionHost>(
        new ExtensionHost(std::move(*host), options.logger, options.workspace));
}

void ExtensionHost::run_step(std::size_t index) {
    const Step& step = startup_steps_[index];
    host_->session().call(
        step.target, step.method, step.args_json, [this, index](CallResult result) {
            if (!result) {
                fail("startup step " + startup_steps_[index].method + " failed: " + result.error());
                return;
            }
            if (index + 1 < startup_steps_.size()) {
                run_step(index + 1);
                return;
            }
            state_ = HostState::ready;
            if (ready_handler_) {
                ready_handler_();
            }
        });
}

void ExtensionHost::fail(std::string message) {
    logger_->log(core::LogLevel::error, "extension host", message);
    state_ = HostState::failed;
    failure_ = std::move(message);
}

void ExtensionHost::abandon(const std::string& reason) {
    state_ = HostState::closed;
    host_->session().fail_pending_calls(reason);
}

std::expected<bool, Error> ExtensionHost::pump(std::chrono::milliseconds timeout) {
    const auto pumped = host_->pump(timeout);
    logging_.poll();
    if (pumped && !*pumped && state_ != HostState::failed) {
        state_ = HostState::closed;
    }
    return pumped;
}

} // namespace anvil::exthost
