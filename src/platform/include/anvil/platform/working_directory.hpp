#pragma once

#include "anvil/core/error.hpp"

#include <expected>
#include <filesystem>
#include <vector>

namespace anvil::platform {

struct WorkingDirectory {
    // The process's working directory after the call.
    std::filesystem::path directory;
    // True when the directory the process started in was unusable and has been replaced.
    bool changed = false;
};

// Makes sure the process has a working directory that exists. A process started from a directory
// that was deleted afterwards, such as a shell whose current directory a rebuild removed, cannot
// resolve paths, and libraries that read it fail in confusing ways (Fontconfig reports "out of
// memory"). When the working directory is unusable the process moves to the first of `fallbacks`
// that can be entered. Fails with ErrorCode::environment_unavailable when none can.
std::expected<WorkingDirectory, Error>
ensure_working_directory(const std::vector<std::filesystem::path>& fallbacks);

// The same with the fallbacks a user expects: the home directory, then the temporary directory.
std::expected<WorkingDirectory, Error> ensure_working_directory();

} // namespace anvil::platform
