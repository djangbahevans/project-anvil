#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/log.hpp"
#include "anvil/core/workspace.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil::exthost {

struct InitDataOptions {
    // The VS Code API version the extension host reports to extensions.
    std::string api_version = "1.146.0";
    std::string app_name = "Anvil";
    std::string uri_scheme = "anvil";
    std::string language = "en";
    // What extensions see as the log level: the numeric value of core::LogLevel.
    int log_level = static_cast<int>(core::LogLevel::info);
    // The transpiled VS Code checkout the extension host runs from.
    std::filesystem::path app_root{};
    // Directories of unpacked extensions, each containing a package.json.
    std::vector<std::filesystem::path> extension_dirs{};
    // Base directory for extension global storage and workspace storage, and for logs unless
    // `logs_dir` is set.
    std::filesystem::path storage_dir{};
    // Where the extension host writes its log files; `storage_dir`/logs when empty.
    std::filesystem::path logs_dir{};
    // The folder extensions work on; they see no workspace when unset.
    std::optional<core::Workspace> workspace{};
};

// The arguments of `ExtHostWorkspace.$initializeWorkspace`, as a JSON array: the workspace data
// (null without a workspace) and whether the workspace is trusted.
std::string workspace_initialization_args(const std::optional<core::Workspace>& workspace);

// Builds the JSON text of the extension host's initialization data, reading each extension's
// manifest. Fails if a manifest is missing or malformed.
std::expected<std::string, Error> build_init_data(const InitDataOptions& options);

} // namespace anvil::exthost
