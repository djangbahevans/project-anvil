#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/workspace.hpp"
#include "anvil/exthost/commands_service.hpp"
#include "anvil/exthost/documents_service.hpp"
#include "anvil/exthost/file_event_service.hpp"
#include "anvil/exthost/file_system_service.hpp"
#include "anvil/exthost/host_process.hpp"
#include "anvil/exthost/logging_service.hpp"
#include "anvil/exthost/request_router.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anvil::exthost {

struct ExtensionHostOptions {
    std::filesystem::path node_executable{};
    // A transpiled VS Code checkout containing out/bootstrap-fork.js (see scripts/fetch-vscode.sh).
    std::filesystem::path vscode_dir{};
    // Directories of unpacked extensions to load.
    std::vector<std::filesystem::path> extension_dirs{};
    // Required. The extension host runs in this directory, keeps extension storage in it, and
    // writes the logs of each session to logs/<session> in it, keeping the newest few sessions.
    std::filesystem::path storage_dir{};
    // Receives the extension host's own messages and everything extensions log. Messages are
    // discarded when unset.
    std::shared_ptr<core::Logger> logger{};
    // The folder extensions work on, trusted. Extensions see no workspace when unset.
    std::optional<core::Workspace> workspace{};
    std::chrono::milliseconds connect_timeout{15000};
};

enum class HostState {
    // Connected; the startup sequence has not completed.
    starting,
    // Eager extensions are activated and commands can run.
    ready,
    // The startup sequence failed; failure() says why.
    failed,
    // The connection closed.
    closed,
};

// An extension host process together with the editor-side services it talks to. start() launches
// the process; the startup sequence then runs as the caller pumps the connection.
class ExtensionHost {
public:
    static std::expected<std::unique_ptr<ExtensionHost>, Error>
    start(const ExtensionHostOptions& options);

    ExtensionHost(const ExtensionHost&) = delete;
    ExtensionHost& operator=(const ExtensionHost&) = delete;

    HostState state() const {
        return state_;
    }
    const std::string& failure() const {
        return failure_;
    }

    // Runs once when the host becomes ready.
    void set_ready_handler(std::function<void()> handler) {
        ready_handler_ = std::move(handler);
    }

    CommandsService& commands() {
        return commands_;
    }

    // The documents and editors that extensions can see.
    DocumentsService& documents() {
        return documents_;
    }

    // Registers handlers for further services before pumping starts.
    RequestRouter& router() {
        return router_;
    }

    // "Service.method" names of requests from the extension host that nothing handled.
    const std::set<std::string>& unhandled_requests() const {
        return router_.unhandled();
    }

    // Gives up on the host: every request that has no reply yet fails with `reason`, and the host
    // is closed. Destroying the host stops the process; this tells the callers of requests why
    // they get no answer.
    void abandon(const std::string& reason);

    // Services the connection for up to `timeout`; see HostProcess::pump. Returns false once the
    // connection has closed.
    std::expected<bool, Error> pump(std::chrono::milliseconds timeout);

private:
    ExtensionHost(std::unique_ptr<HostProcess> host, std::shared_ptr<core::Logger> logger,
                  std::optional<core::Workspace> workspace);

    struct Step {
        ProxyId target;
        std::string method;
        std::string args_json;
    };
    void run_step(std::size_t index);
    void fail(std::string message);

    // Declaration order matters: the commands service uses the session inside host_ and the
    // router, so it is destroyed first.
    RequestRouter router_;
    std::shared_ptr<core::Logger> logger_;
    std::unique_ptr<HostProcess> host_;
    CommandsService commands_;
    DocumentsService documents_;
    LoggingService logging_;
    FileSystemService file_system_;
    FileEventService file_events_;
    std::optional<core::Workspace> workspace_;
    std::vector<Step> startup_steps_;
    HostState state_ = HostState::starting;
    std::string failure_;
    std::function<void()> ready_handler_;
};

} // namespace anvil::exthost
