#pragma once

#include "anvil/core/error.hpp"
#include "anvil/exthost/session.hpp"
#include "anvil/platform/local_socket.hpp"
#include "anvil/platform/process.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace anvil::exthost {

struct HostOptions {
    // The Node.js binary that runs the extension host.
    std::filesystem::path node_executable;
    // A transpiled VS Code checkout containing out/bootstrap-fork.js (see scripts/fetch-vscode.sh).
    std::filesystem::path vscode_dir;
    // JSON text of the extension host's IExtensionHostInitData.
    std::string init_data_json;
    std::chrono::milliseconds connect_timeout{15000};
};

// A running extension host process connected to a Session. The caller drives it by calling pump()
// repeatedly from one thread; the session's handlers run inside pump().
class HostProcess {
public:
    // Starts the extension host and waits for it to connect to the editor's socket.
    static std::expected<std::unique_ptr<HostProcess>, Error> start(const HostOptions& options);

    ~HostProcess();
    HostProcess(const HostProcess&) = delete;
    HostProcess& operator=(const HostProcess&) = delete;

    Session& session() {
        return session_;
    }
    platform::Process& process() {
        return process_;
    }

    // Waits up to `timeout` for data from the extension host, feeds it to the session, and writes
    // out what the session queued. Returns false once the connection has closed.
    std::expected<bool, Error> pump(std::chrono::milliseconds timeout);

private:
    HostProcess(std::string init_data_json, std::filesystem::path socket_directory,
                platform::Process process, platform::LocalConnection connection);

    Session session_;
    std::filesystem::path socket_directory_;
    platform::Process process_;
    platform::LocalConnection connection_;
    bool closed_ = false;
};

// The environment for the extension host: the current environment without the Electron and VS
// Code variables that switch its module loader to Electron-only behavior, plus the entries the
// extension host needs to connect to `socket_path`.
std::vector<std::string> extension_host_environment(const std::vector<std::string>& inherited,
                                                    const std::filesystem::path& socket_path);

} // namespace anvil::exthost
