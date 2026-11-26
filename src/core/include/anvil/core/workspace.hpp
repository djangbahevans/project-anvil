#pragma once

#include "anvil/core/error.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil::core {

// The folder the editor works on. Extensions see it as the workspace folder, and the workspace
// storage of an extension is kept per workspace id.
struct Workspace {
    // The canonical absolute path of the folder, without a trailing separator.
    std::filesystem::path folder;
    // What the editor shows for the workspace: the folder's name, or its path for a root.
    std::string name;
    // 16 lowercase hexadecimal digits: the 64-bit FNV-1a hash of the UTF-8 bytes of `folder` in
    // its generic form (forward slashes). The same folder has the same id in every run and on
    // every platform, which `std::hash` does not guarantee.
    std::string id;

    // Fails when `folder` is not an existing directory. A symbolic link to a directory names the
    // directory it points to, so both paths open the same workspace.
    static std::expected<Workspace, Error> open(const std::filesystem::path& folder);

    auto operator<=>(const Workspace&) const = default;
};

// What the command line asks the editor to open: at most one workspace folder and any number of
// files.
struct LaunchArguments {
    std::optional<Workspace> workspace;
    std::vector<std::filesystem::path> files;
    // What was wrong with the arguments that were ignored, one message each.
    std::vector<std::string> problems;
    // `--new-window` or `-n`: the window is to start empty, not with the folder open last.
    bool new_window = false;

    // Whether the arguments leave it open which folder the window starts with: they name none,
    // none that could not be opened, and do not ask for an empty window.
    bool may_reopen_folder() const {
        return !workspace && problems.empty() && !new_window;
    }
};

// `--new-window` and `-n` are options. Of the other arguments, the first that names an existing
// directory is the workspace folder. An argument that ends in a separator names a folder too, so
// one that does not exist is reported instead of being opened as a new file, as is every directory
// after the first. All the rest are files, whether or not they exist.
LaunchArguments parse_launch_arguments(const std::vector<std::string>& arguments);

} // namespace anvil::core
