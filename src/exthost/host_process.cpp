#include "anvil/exthost/host_process.hpp"

#include <array>
#include <random>

namespace anvil::exthost {
namespace {

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

// A short directory name keeps the socket path within the Unix socket length limit.
std::expected<std::filesystem::path, Error> create_socket_directory() {
    constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device random;
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::string name = "anvil-";
        for (int i = 0; i < 8; ++i) {
            name += alphabet[random() % alphabet.size()];
        }
        std::error_code error;
        const auto path = std::filesystem::temp_directory_path(error) / name;
        if (error) {
            return std::unexpected(
                Error{ErrorCode::io_error, "no temporary directory: " + error.message()});
        }
        if (std::filesystem::create_directory(path, error)) {
            std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
            return path;
        }
    }
    return std::unexpected(Error{ErrorCode::io_error, "cannot create a socket directory"});
}

} // namespace

std::vector<std::string> extension_host_environment(const std::vector<std::string>& inherited,
                                                    const std::filesystem::path& socket_path) {
    std::vector<std::string> environment;
    for (const auto& entry : inherited) {
        if (starts_with(entry, "ELECTRON_RUN_AS_NODE=") || starts_with(entry, "VSCODE_")) {
            continue;
        }
        environment.push_back(entry);
    }
    environment.push_back("VSCODE_ESM_ENTRYPOINT=vs/workbench/api/node/extensionHostProcess");
    environment.push_back("VSCODE_EXTHOST_IPC_HOOK=" + socket_path.string());
    environment.push_back("VSCODE_HANDLES_UNCAUGHT_ERRORS=true");
    return environment;
}

HostProcess::HostProcess(std::string init_data_json, std::filesystem::path socket_directory,
                         platform::Process process, platform::LocalConnection connection)
    : session_(std::move(init_data_json)), socket_directory_(std::move(socket_directory)),
      process_(std::move(process)), connection_(std::move(connection)) {}

std::expected<std::unique_ptr<HostProcess>, Error> HostProcess::start(const HostOptions& options) {
    const auto bootstrap = options.vscode_dir / "out" / "bootstrap-fork.js";
    if (!std::filesystem::exists(bootstrap)) {
        return std::unexpected(Error{ErrorCode::io_error, "missing " + bootstrap.string() +
                                                              "; run scripts/fetch-vscode.sh"});
    }

    auto directory = create_socket_directory();
    if (!directory) {
        return std::unexpected(directory.error());
    }
    // The listener removes its socket file; this removes the directory once start() is done with
    // it.
    struct DirectoryCleanup {
        std::filesystem::path path;
        bool release = false;
        ~DirectoryCleanup() {
            if (!release) {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        }
    } cleanup{*directory};

    const auto socket_path = *directory / "host.sock";
    auto listener = platform::LocalListener::listen(socket_path);
    if (!listener) {
        return std::unexpected(listener.error());
    }

    auto process = platform::Process::spawn(platform::ProcessSpec{
        options.node_executable,
        {bootstrap.string(), "--type=extensionHost"},
        extension_host_environment(platform::current_environment(), socket_path),
    });
    if (!process) {
        return std::unexpected(process.error());
    }

    const auto deadline = std::chrono::steady_clock::now() + options.connect_timeout;
    while (true) {
        auto accepted = listener->accept(std::chrono::milliseconds(100));
        if (!accepted) {
            return std::unexpected(accepted.error());
        }
        if (*accepted) {
            cleanup.release = true;
            return std::unique_ptr<HostProcess>(new HostProcess(
                options.init_data_json, *directory, std::move(*process), std::move(**accepted)));
        }
        if (const auto exit_code = process->wait(std::chrono::milliseconds(0))) {
            return std::unexpected(
                Error{ErrorCode::io_error, "the extension host exited with code " +
                                               std::to_string(*exit_code) + " before connecting"});
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(
                Error{ErrorCode::io_error, "the extension host did not connect in time"});
        }
    }
}

HostProcess::~HostProcess() {
    session_.terminate();
    if (const Bytes output = session_.take_output(); !output.empty()) {
        (void)connection_.write_all(output);
    }
    if (!process_.wait(std::chrono::milliseconds(2000))) {
        process_.kill();
    }
    std::error_code ignored;
    std::filesystem::remove_all(socket_directory_, ignored);
}

std::expected<bool, Error> HostProcess::pump(std::chrono::milliseconds timeout) {
    if (closed_) {
        return false;
    }
    if (const Bytes output = session_.take_output(); !output.empty()) {
        if (auto written = connection_.write_all(output); !written) {
            return std::unexpected(written.error());
        }
    }

    std::array<std::byte, 64 * 1024> buffer;
    auto read = connection_.read(buffer, timeout);
    if (!read) {
        return std::unexpected(read.error());
    }
    if (read->bytes > 0) {
        if (auto received = session_.receive(std::span{buffer}.first(read->bytes)); !received) {
            closed_ = true;
            return std::unexpected(received.error());
        }
        if (const Bytes output = session_.take_output(); !output.empty()) {
            if (auto written = connection_.write_all(output); !written) {
                return std::unexpected(written.error());
            }
        }
    }
    if (read->closed || session_.state() == SessionState::closed) {
        closed_ = true;
    }
    return !closed_;
}

} // namespace anvil::exthost
