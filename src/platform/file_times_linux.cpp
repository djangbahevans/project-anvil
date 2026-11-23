#include "file_times.hpp"

#include <fcntl.h>
#include <sys/stat.h>

namespace anvil::platform {

std::optional<FileTimes> file_times(const std::filesystem::path& path, bool follow) {
    struct statx result {};
    const int flags = follow ? AT_STATX_SYNC_AS_STAT : AT_SYMLINK_NOFOLLOW;
    if (::statx(AT_FDCWD, path.c_str(), flags, STATX_MTIME | STATX_BTIME, &result) != 0) {
        return std::nullopt;
    }
    const auto milliseconds = [](const statx_timestamp& time) {
        return static_cast<std::int64_t>(time.tv_sec) * 1000 + time.tv_nsec / 1'000'000;
    };
    FileTimes times;
    times.modified_ms = milliseconds(result.stx_mtime);
    if (result.stx_mask & STATX_BTIME) {
        times.created_ms = milliseconds(result.stx_btime);
    }
    return times;
}

} // namespace anvil::platform
