#pragma once

#include "anvil/core/error.hpp"

#include <expected>
#include <filesystem>

namespace anvil::platform {

// Per-user directory for editor configuration:
//   Linux:   $XDG_CONFIG_HOME/anvil, or $HOME/.config/anvil
//   macOS:   $HOME/Library/Application Support/anvil
//   Windows: %APPDATA%\anvil
// Fails with ErrorCode::environment_unavailable if the base location cannot be determined.
std::expected<std::filesystem::path, Error> user_config_dir();

} // namespace anvil::platform
