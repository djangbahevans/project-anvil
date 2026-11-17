#include "anvil/platform/paths.hpp"

#include <cstdlib>

namespace anvil::platform {
namespace {

std::filesystem::path absolute_env_path(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return {};
    }
    std::filesystem::path path{value};
    return path.is_absolute() ? path : std::filesystem::path{};
}

} // namespace

std::expected<std::filesystem::path, Error> user_config_dir() {
    if (auto xdg = absolute_env_path("XDG_CONFIG_HOME"); !xdg.empty()) {
        return xdg / "anvil";
    }
    if (auto home = absolute_env_path("HOME"); !home.empty()) {
        return home / ".config" / "anvil";
    }
    return std::unexpected(Error{
        ErrorCode::environment_unavailable,
        "neither XDG_CONFIG_HOME nor HOME is set to an absolute path",
    });
}

} // namespace anvil::platform
