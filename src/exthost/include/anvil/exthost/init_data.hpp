#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/log.hpp"

#include <expected>
#include <filesystem>
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
    // Base directory for extension global storage, workspace storage and logs.
    std::filesystem::path storage_dir{};
};

// Builds the JSON text of the extension host's initialization data, reading each extension's
// manifest. Fails if a manifest is missing or malformed.
std::expected<std::string, Error> build_init_data(const InitDataOptions& options);

} // namespace anvil::exthost
