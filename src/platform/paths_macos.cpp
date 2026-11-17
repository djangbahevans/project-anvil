#include "anvil/platform/paths.hpp"

#include <cstdlib>

namespace anvil::platform {

std::expected<std::filesystem::path, Error> user_config_dir() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return std::unexpected(Error{
            ErrorCode::environment_unavailable,
            "HOME is not set",
        });
    }
    return std::filesystem::path{home} / "Library" / "Application Support" / "anvil";
}

} // namespace anvil::platform
