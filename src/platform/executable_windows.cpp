#include "anvil/platform/executable.hpp"

// Running programs is not implemented on Windows yet, so none is found.
namespace anvil::platform {

bool is_executable(const std::filesystem::path&) {
    return false;
}

std::optional<std::filesystem::path> find_executable(std::string_view, const std::string&) {
    return std::nullopt;
}

} // namespace anvil::platform
