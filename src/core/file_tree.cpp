#include "anvil/core/file_tree.hpp"

#include "anvil/core/file_name_order.hpp"
#include "anvil/core/glob.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace anvil::core {

namespace {

std::string join(const std::string& directory, const std::string& name) {
    return directory.empty() ? name : directory + "/" + name;
}

// Puts what holds an entry in the order of the explorer: directories first, then by name.
template <typename T, typename EntryOf>
void sort_in_order(std::vector<T>& items, EntryOf entry_of) {
    std::vector<std::pair<FileNameKey, std::size_t>> keyed;
    keyed.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        keyed.emplace_back(FileNameKey(entry_of(items[i]).name), i);
    }
    std::ranges::sort(keyed, [&](const auto& a, const auto& b) {
        const bool a_directory = entry_of(items[a.second]).is_directory;
        const bool b_directory = entry_of(items[b.second]).is_directory;
        if (a_directory != b_directory) {
            return a_directory;
        }
        return a.first < b.first;
    });
    std::vector<T> sorted;
    sorted.reserve(items.size());
    for (const auto& [name_key, index] : keyed) {
        sorted.push_back(std::move(items[index]));
    }
    items = std::move(sorted);
}

} // namespace

const std::vector<FileTreeEntry> FileTree::no_entries_;

FileExcludes FileExcludes::defaults() {
    return FileExcludes{
        .patterns = {"**/.git", "**/.svn", "**/.hg", "**/.jj", "**/.DS_Store", "**/Thumbs.db"},
        .problems = {},
    };
}

FileExcludes FileExcludes::from_setting(const nlohmann::json& setting) {
    FileExcludes result;
    if (!setting.is_object()) {
        result.problems.push_back("files.exclude is not an object");
        return result;
    }
    for (const auto& [pattern, value] : setting.items()) {
        if (value.is_boolean()) {
            if (value.get<bool>()) {
                result.patterns.push_back(pattern);
            }
        } else if (value.is_object()) {
            result.problems.push_back("files.exclude: the condition of \"" + pattern +
                                      "\" is not supported");
        } else {
            result.problems.push_back("files.exclude: the value of \"" + pattern +
                                      "\" is not true or false");
        }
    }
    return result;
}

FileTree::FileTree(std::filesystem::path root, DirectoryLister list, FileExcludes excludes)
    : root_(root.lexically_normal()), list_(std::move(list)), excludes_(std::move(excludes)) {
    if (!root_.has_filename() && root_ != root_.root_path()) {
        root_ = root_.parent_path();
    }
}

bool FileTree::is_excluded(const std::string& relative_path) const {
    return std::ranges::any_of(excludes_.patterns, [&](const std::string& pattern) {
        return glob_match(pattern, relative_path);
    });
}

std::optional<std::string> FileTree::key_of(const std::filesystem::path& directory) const {
    const std::filesystem::path relative = directory.lexically_normal().lexically_relative(root_);
    if (relative.empty()) {
        return std::nullopt;
    }
    std::string key;
    for (const std::filesystem::path& part : relative) {
        const std::string name = part.generic_string();
        if (name == "..") {
            return std::nullopt;
        }
        if (name.empty() || name == ".") {
            continue;
        }
        key = join(key, name);
        if (is_excluded(key)) {
            return std::nullopt;
        }
    }
    return key;
}

std::filesystem::path FileTree::directory_of(const std::string& key) const {
    return key.empty() ? root_ : root_ / key;
}

FileTree::Directory FileTree::read(const std::filesystem::path& directory,
                                   const std::string& key) const {
    Directory result;
    auto listed = list_(directory);
    if (!listed) {
        result.error = std::move(listed.error());
        return result;
    }
    for (FileTreeEntry& entry : *listed) {
        if (!is_excluded(join(key, entry.name))) {
            result.entries.push_back(std::move(entry));
        }
    }
    sort_in_order(result.entries,
                  [](const FileTreeEntry& entry) -> const FileTreeEntry& { return entry; });
    return result;
}

const std::vector<FileTreeEntry>& FileTree::children(const std::filesystem::path& directory) {
    const std::optional<std::string> key = key_of(directory);
    if (!key) {
        return no_entries_;
    }
    auto found = loaded_.find(*key);
    if (found == loaded_.end()) {
        found = loaded_.emplace(*key, read(directory_of(*key), *key)).first;
    }
    return found->second.entries;
}

const Error* FileTree::error(const std::filesystem::path& directory) const {
    const std::optional<std::string> key = key_of(directory);
    if (!key) {
        return nullptr;
    }
    const auto found = loaded_.find(*key);
    return found != loaded_.end() && found->second.error ? &*found->second.error : nullptr;
}

bool FileTree::is_loaded(const std::filesystem::path& directory) const {
    const std::optional<std::string> key = key_of(directory);
    return key && loaded_.contains(*key);
}

void FileTree::forget_below(const std::string& key) {
    const std::string prefix = key + "/";
    std::erase_if(loaded_, [&](const auto& item) {
        return item.first == key || item.first.starts_with(prefix);
    });
}

std::vector<FileTreeChange> FileTree::refresh(const std::filesystem::path& directory) {
    const std::optional<std::string> key = key_of(directory);
    if (!key) {
        return {};
    }
    const auto found = loaded_.find(*key);
    if (found == loaded_.end()) {
        return {};
    }
    Directory fresh = read(directory_of(*key), *key);
    const std::vector<FileTreeEntry> before = std::move(found->second.entries);

    std::vector<FileTreeChange> changes;
    for (const FileTreeEntry& old_entry : before) {
        const auto same_name =
            std::ranges::find(fresh.entries, old_entry.name, &FileTreeEntry::name);
        if (same_name == fresh.entries.end()) {
            changes.push_back({FileTreeChange::Kind::removed, old_entry});
            forget_below(join(*key, old_entry.name));
        } else if (*same_name != old_entry) {
            changes.push_back({FileTreeChange::Kind::changed, *same_name});
            forget_below(join(*key, old_entry.name));
        }
    }
    for (const FileTreeEntry& new_entry : fresh.entries) {
        if (std::ranges::find(before, new_entry.name, &FileTreeEntry::name) == before.end()) {
            changes.push_back({FileTreeChange::Kind::added, new_entry});
        }
    }
    sort_in_order(
        changes, [](const FileTreeChange& change) -> const FileTreeEntry& { return change.entry; });
    loaded_.insert_or_assign(*key, std::move(fresh));
    return changes;
}

} // namespace anvil::core
