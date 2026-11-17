#pragma once

#include <compare>
#include <filesystem>
#include <optional>
#include <string>

namespace anvil::core {

// A resource identifier in the shape VS Code uses: components are held decoded. File resources
// have the scheme "file" and an absolute path with forward slashes; a Windows drive letter is
// written as "/c:/dir/file".
struct Uri {
    std::string scheme;
    std::string authority;
    std::string path;
    std::string query;
    std::string fragment;

    static Uri file(const std::filesystem::path& path);

    // The file system path of a "file" resource, or std::nullopt for other schemes.
    std::optional<std::filesystem::path> file_path() const;

    // A string that identifies the resource, such as "file:///home/me/a.txt".
    std::string to_string() const;

    auto operator<=>(const Uri&) const = default;
};

} // namespace anvil::core
