#include "file_times.hpp"

#include <sys/stat.h>

namespace anvil::platform {

// Not built or run yet: the macOS preset has not been built.
std::optional<FileTimes> file_times(const std::filesystem::path& path, bool follow) {
    struct stat result {};
    if ((follow ? ::stat(path.c_str(), &result) : ::lstat(path.c_str(), &result)) != 0) {
        return std::nullopt;
    }
    const auto milliseconds = [](const timespec& time) {
        return static_cast<std::int64_t>(time.tv_sec) * 1000 + time.tv_nsec / 1'000'000;
    };
    return FileTimes{.modified_ms = milliseconds(result.st_mtimespec),
                     .created_ms = milliseconds(result.st_birthtimespec)};
}

} // namespace anvil::platform
