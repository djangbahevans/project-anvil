#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace anvil::platform {

struct FileTimes {
    // Milliseconds since the Unix epoch.
    std::int64_t modified_ms = 0;
    // Empty when the operating system or the file system records no creation time.
    std::optional<std::int64_t> created_ms;
};

// The times of a file, or nothing when they cannot be read. With `follow` a symbolic link is looked
// through. One implementation per operating system.
std::optional<FileTimes> file_times(const std::filesystem::path& path, bool follow);

} // namespace anvil::platform
