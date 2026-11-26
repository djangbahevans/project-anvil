#include "anvil/platform/executable.hpp"

#include <system_error>
#include <unistd.h>

namespace anvil::platform {

bool is_executable(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && ::access(path.c_str(), X_OK) == 0;
}

std::optional<std::filesystem::path> find_executable(std::string_view name,
                                                     const std::string& path_variable) {
    std::string_view rest = path_variable;
    while (!rest.empty()) {
        const std::size_t colon = rest.find(':');
        const std::filesystem::path directory(rest.substr(0, colon));
        rest.remove_prefix(colon == std::string_view::npos ? rest.size() : colon + 1);
        if (directory.is_absolute() && is_executable(directory / name)) {
            return directory / name;
        }
    }
    return std::nullopt;
}

} // namespace anvil::platform
