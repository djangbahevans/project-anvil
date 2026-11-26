#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace anvil::platform {

// Whether the path is a file that this process may run.
bool is_executable(const std::filesystem::path& path);

// The first file of the name that can be run in the directories of a `PATH` value, as an absolute
// path. Empty and relative entries, which would depend on the working directory, are passed over.
std::optional<std::filesystem::path> find_executable(std::string_view name,
                                                     const std::string& path_variable);

} // namespace anvil::platform
