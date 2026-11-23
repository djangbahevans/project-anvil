#pragma once

#include "anvil/core/error.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace anvil::platform {

// The type of a file system entry as bit flags, with the values that VS Code uses (`FileType`). A
// symbolic link has its own bit added to the type of its target, and is the bit alone when the
// target does not exist.
namespace file_type {
inline constexpr unsigned file = 1;
inline constexpr unsigned directory = 2;
inline constexpr unsigned symbolic_link = 64;
} // namespace file_type

struct FileStat {
    unsigned type = 0;
    // In bytes; zero for a directory.
    std::uint64_t size = 0;
    // Milliseconds since the Unix epoch.
    std::int64_t modified_ms = 0;
    // The creation time that the operating system records (`statx` on Linux, `st_birthtime` on
    // macOS, the creation time on Windows), and the modification time where it records none. The
    // macOS and Windows implementations have not been built.
    std::int64_t created_ms = 0;
    // The owner cannot write the file.
    bool read_only = false;
};

// Describes `path`. A symbolic link is described by what it points to, with
// `file_type::symbolic_link` added to the type; a broken link has that type alone and the times of
// the link.
std::expected<FileStat, Error> stat_path(const std::filesystem::path& path);

struct DirectoryEntry {
    // The name as it is on disk: a byte string that need not be valid UTF-8 (a name on Windows is
    // UTF-8).
    std::string name;
    // As for `stat_path`.
    unsigned type = 0;
};

// The entries of a directory sorted by name, without `.` and `..`.
std::expected<std::vector<DirectoryEntry>, Error> list_directory(const std::filesystem::path& path);

// Creates the directory and its missing parents. Succeeds when it exists as a directory, and fails
// with `already_exists` when a file is in the way.
std::expected<void, Error> make_directory(const std::filesystem::path& path);

// Removes a file, a symbolic link (never its target), an empty directory, or with `recursive` a
// directory and everything in it, without following symbolic links out of it. A non-empty
// directory without `recursive` fails.
std::expected<void, Error> remove_path(const std::filesystem::path& path, bool recursive);

// Moves `from` to `to` on the same file system. Fails with `already_exists` when `to` exists and
// `overwrite` is false; with `overwrite` an existing target, a directory included, is removed
// first when it cannot be replaced directly. A move to another file system fails with `io_error`.
std::expected<void, Error> rename_path(const std::filesystem::path& from,
                                       const std::filesystem::path& to, bool overwrite);

// Copies a file, or a directory with everything in it; a symbolic link is copied as a link. Fails
// with `already_exists` when `to` exists and `overwrite` is false; with `overwrite` an existing
// target is removed first. Copying a directory into itself fails.
std::expected<void, Error> copy_path(const std::filesystem::path& from,
                                     const std::filesystem::path& to, bool overwrite);

} // namespace anvil::platform
