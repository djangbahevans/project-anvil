#include "file_times.hpp"

#include <windows.h>

namespace anvil::platform {

// Not built or run yet: the Windows preset has not been built. The attributes of a symbolic link
// are those of the link itself, so `follow` does not change what is returned.
std::optional<FileTimes> file_times(const std::filesystem::path& path, bool) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }
    constexpr std::uint64_t windows_epoch_to_unix = 116444736000000000ULL;
    const auto milliseconds = [&](const FILETIME& time) {
        ULARGE_INTEGER value;
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return static_cast<std::int64_t>((value.QuadPart - windows_epoch_to_unix) / 10000);
    };
    return FileTimes{.modified_ms = milliseconds(data.ftLastWriteTime),
                     .created_ms = milliseconds(data.ftCreationTime)};
}

} // namespace anvil::platform
