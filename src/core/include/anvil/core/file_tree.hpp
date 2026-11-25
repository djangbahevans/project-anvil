#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/settings.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace anvil::core {

// One name in a directory, as the platform lists it.
struct FileTreeEntry {
    std::string name;
    // The entry is a directory, or a symbolic link whose target is one.
    bool is_directory = false;
    bool is_symbolic_link = false;

    bool operator==(const FileTreeEntry&) const = default;
};

// Lists the entries of a directory in any order. Fails with the reason that the explorer shows.
using DirectoryLister =
    std::function<std::expected<std::vector<FileTreeEntry>, Error>(const std::filesystem::path&)>;

// Patterns of `files.exclude`: globs that are matched against the path of an entry relative to the
// root of the tree, with forward slashes. An entry that matches is not in the tree, and neither is
// anything under it.
struct FileExcludes {
    std::vector<std::string> patterns;
    // What was wrong with the setting, one message per ignored entry.
    std::vector<std::string> problems;

    // VS Code's default: `**/.git`, `**/.svn`, `**/.hg`, `**/.jj`, `**/.DS_Store`,
    // `**/Thumbs.db`.
    static FileExcludes defaults();

    // Reads the object of the setting: each key is a glob, and the value `true` turns it on and
    // `false` off. A value that is an object (a `when` condition) is not supported and is reported
    // as a problem, as is any other value or a setting that is not an object.
    static FileExcludes from_setting(const nlohmann::json& setting);

    // The patterns that are on once the user's `files.exclude` is laid over VS Code's default
    // object, as VS Code merges the two: an entry of the setting turns a default pattern on or off
    // and adds the others, and a default that the setting does not mention stays. Without the
    // setting that is `defaults()`. A value that is not usable is reported as in `from_setting`.
    static FileExcludes from_settings(const Settings& settings);
};

// A change that reading a directory again found.
struct FileTreeChange {
    enum class Kind {
        added,
        removed,
        // The name is still there and is a directory where it was a file or the other way
        // round, or its symbolic link flag differs.
        changed,
    };
    Kind kind = Kind::added;
    // The entry as it is after the change; the entry as it was for `removed`.
    FileTreeEntry entry;

    bool operator==(const FileTreeChange&) const = default;
};

// The directories of a folder that have been read so far, as the explorer shows them.
//
// A directory is listed when `children` first asks for it and never again until `refresh`, so a
// large tree costs only what the user opens. Entries are sorted as the explorer of VS Code sorts
// them: directories first, then each group by `compare_file_names`. A symbolic link to a directory
// is a directory entry that the tree does not follow by itself: its contents are read only when
// they are asked for, so a link loop is as deep as the user opens it.
class FileTree {
public:
    FileTree(std::filesystem::path root, DirectoryLister list,
             FileExcludes excludes = FileExcludes::defaults());

    const std::filesystem::path& root() const {
        return root_;
    }

    // The entries of `directory`, which is the root or a path below it, in order. Lists the
    // directory the first time. A directory that is excluded, lies outside the root or fails to
    // list has none; `error` tells the last. The reference stays valid until the next `refresh`.
    const std::vector<FileTreeEntry>& children(const std::filesystem::path& directory);

    // Why the last listing of `directory` failed; null when it was listed, is not loaded or lies
    // outside the tree.
    const Error* error(const std::filesystem::path& directory) const;

    // Replaces the patterns without listing anything. What was listed under the old ones stays as
    // it is until `refresh` reads it again.
    void set_excludes(FileExcludes excludes) {
        excludes_ = std::move(excludes);
    }

    // Whether `children` has listed `directory` and the listing has not been dropped.
    bool is_loaded(const std::filesystem::path& directory) const;

    // Lists a loaded directory again and returns what differs from before, in the order of the
    // entries. The directories below an entry that was removed or changed are forgotten, so that
    // a new directory of the same name is listed afresh. Returns nothing for a directory that is
    // not loaded.
    std::vector<FileTreeChange> refresh(const std::filesystem::path& directory);

private:
    struct Directory {
        std::vector<FileTreeEntry> entries;
        std::optional<Error> error;
    };

    // The key of `directory` in `loaded_`: its path relative to the root with forward slashes, or
    // none when it lies outside the root or is excluded or below an excluded directory.
    std::optional<std::string> key_of(const std::filesystem::path& directory) const;
    std::filesystem::path directory_of(const std::string& key) const;
    Directory read(const std::filesystem::path& directory, const std::string& key) const;
    void forget_below(const std::string& key);
    bool is_excluded(const std::string& relative_path) const;

    std::filesystem::path root_;
    DirectoryLister list_;
    FileExcludes excludes_;
    std::unordered_map<std::string, Directory> loaded_;
    static const std::vector<FileTreeEntry> no_entries_;
};

} // namespace anvil::core
