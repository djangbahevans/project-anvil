#include "anvil/core/file_tree.hpp"

#include "anvil/core/file_name_order.hpp"
#include "anvil/core/glob.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string_view>
#include <unordered_set>

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

namespace {

// The entries of the setting that say on or off, in the order of the setting. `key` names the
// setting in the problems.
std::vector<std::pair<std::string, bool>> switches_of(const nlohmann::json& setting,
                                                      const std::string& key,
                                                      std::vector<std::string>& problems) {
    std::vector<std::pair<std::string, bool>> result;
    if (!setting.is_object()) {
        problems.push_back(key + " is not an object");
        return result;
    }
    for (const auto& [pattern, value] : setting.items()) {
        if (value.is_boolean()) {
            result.emplace_back(pattern, value.get<bool>());
        } else if (value.is_object()) {
            problems.push_back(key + ": the condition of \"" + pattern + "\" is not supported");
        } else {
            problems.push_back(key + ": the value of \"" + pattern + "\" is not true or false");
        }
    }
    return result;
}

// Turns patterns on or off as the entries say.
void apply_switches(std::vector<std::string>& patterns,
                    std::vector<std::pair<std::string, bool>> switches) {
    for (auto& [pattern, on] : switches) {
        const auto present = std::ranges::find(patterns, pattern);
        if (on && present == patterns.end()) {
            patterns.push_back(std::move(pattern));
        } else if (!on && present != patterns.end()) {
            patterns.erase(present);
        }
    }
}

} // namespace

FileExcludes FileExcludes::from_setting(const nlohmann::json& setting) {
    FileExcludes result;
    for (auto& [pattern, on] : switches_of(setting, "files.exclude", result.problems)) {
        if (on) {
            result.patterns.push_back(std::move(pattern));
        }
    }
    return result;
}

FileExcludes FileExcludes::from_settings(const Settings& settings) {
    FileExcludes result = defaults();
    const nlohmann::json* setting = settings.find("files.exclude");
    if (setting == nullptr) {
        return result;
    }
    apply_switches(result.patterns, switches_of(*setting, "files.exclude", result.problems));
    return result;
}

FileSearchOptions FileSearchOptions::from_settings(const Settings& settings) {
    FileSearchOptions result;
    FileExcludes files = FileExcludes::from_settings(settings);
    result.excludes = std::move(files.patterns);
    result.problems = std::move(files.problems);

    // `search.exclude` lies over `files.exclude`: its defaults and its entries win.
    std::vector<std::pair<std::string, bool>> search = {
        {"**/node_modules", true}, {"**/bower_components", true}, {"**/*.code-search", true}};
    if (const nlohmann::json* setting = settings.find("search.exclude");
        setting != nullptr && !setting->is_null()) {
        for (auto& [pattern, on] : switches_of(*setting, "search.exclude", result.problems)) {
            const auto present =
                std::ranges::find(search, pattern, [](const auto& entry) -> const std::string& {
                    return entry.first;
                });
            if (present != search.end()) {
                present->second = on;
            } else {
                search.emplace_back(std::move(pattern), on);
            }
        }
    }
    apply_switches(result.excludes, std::move(search));

    for (const auto& [key, field] :
         {std::pair{"search.useIgnoreFiles", &FileSearchOptions::use_ignore_files},
          std::pair{"search.useGlobalIgnoreFiles", &FileSearchOptions::use_global_ignore_files},
          std::pair{"search.useParentIgnoreFiles", &FileSearchOptions::use_parent_ignore_files},
          std::pair{"search.followSymlinks", &FileSearchOptions::follow_symlinks}}) {
        const nlohmann::json* setting = settings.find(key);
        if (setting == nullptr || setting->is_null()) {
            continue;
        }
        if (setting->is_boolean()) {
            result.*field = setting->get<bool>();
        } else {
            result.problems.push_back(std::string(key) + " must be true or false");
        }
    }
    return result;
}

DirectoryReader::DirectoryReader(std::filesystem::path root, DirectoryLister list,
                                 FileExcludes excludes)
    : root_(root.lexically_normal()), list_(std::move(list)), excludes_(std::move(excludes)) {
    if (!root_.has_filename() && root_ != root_.root_path()) {
        root_ = root_.parent_path();
    }
}

DirectoryReader DirectoryReader::with_excludes(FileExcludes excludes) const {
    return DirectoryReader(root_, list_, std::move(excludes));
}

bool DirectoryReader::is_excluded(const std::string& relative_path) const {
    return std::ranges::any_of(excludes_.patterns, [&](const std::string& pattern) {
        return glob_match(pattern, relative_path);
    });
}

std::optional<std::string> DirectoryReader::key_of(const std::filesystem::path& directory) const {
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

std::optional<FileTreeListing> DirectoryReader::read(const std::filesystem::path& directory,
                                                     std::stop_token stop) const {
    const std::optional<std::string> key = key_of(directory);
    if (!key) {
        return FileTreeListing{};
    }
    FileTreeListing result;
    auto listed = list_(key->empty() ? root_ : root_ / *key);
    if (stop.stop_requested()) {
        return std::nullopt;
    }
    if (!listed) {
        result.error = std::move(listed.error());
        return result;
    }
    for (FileTreeEntry& entry : *listed) {
        if (!is_excluded(join(*key, entry.name))) {
            result.entries.push_back(std::move(entry));
        }
    }
    sort_in_order(result.entries,
                  [](const FileTreeEntry& entry) -> const FileTreeEntry& { return entry; });
    return result;
}

FileTree::FileTree(std::filesystem::path root, DirectoryLister list, FileExcludes excludes)
    : reader_(std::move(root), std::move(list), std::move(excludes)) {}

std::optional<std::string> FileTree::key_of(const std::filesystem::path& directory) const {
    return reader_.key_of(directory);
}

std::filesystem::path FileTree::directory_of(const std::string& key) const {
    return key.empty() ? reader_.root() : reader_.root() / key;
}

bool FileTree::adopt(const std::filesystem::path& directory, FileTreeListing listing) {
    const std::optional<std::string> key = key_of(directory);
    if (!key) {
        return false;
    }
    loaded_.insert_or_assign(*key, std::move(listing));
    return true;
}

void FileTree::forget(const std::filesystem::path& directory) {
    if (const std::optional<std::string> key = key_of(directory)) {
        forget_below(*key);
    }
}

const std::vector<FileTreeEntry>& FileTree::children(const std::filesystem::path& directory) {
    const std::optional<std::string> key = key_of(directory);
    if (!key) {
        return no_entries_;
    }
    auto found = loaded_.find(*key);
    if (found == loaded_.end()) {
        found = loaded_.emplace(*key, *reader_.read(directory_of(*key))).first;
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
    FileTreeListing fresh = *reader_.read(directory_of(*key));
    const std::vector<FileTreeEntry> before = std::move(found->second.entries);

    // By name, so that a directory of tens of thousands of entries is compared in one pass.
    std::unordered_map<std::string_view, const FileTreeEntry*> fresh_by_name;
    fresh_by_name.reserve(fresh.entries.size());
    for (const FileTreeEntry& entry : fresh.entries) {
        fresh_by_name.emplace(entry.name, &entry);
    }
    std::unordered_set<std::string_view> before_names;
    before_names.reserve(before.size());

    std::vector<FileTreeChange> changes;
    for (const FileTreeEntry& old_entry : before) {
        before_names.insert(old_entry.name);
        const auto same_name = fresh_by_name.find(old_entry.name);
        if (same_name == fresh_by_name.end()) {
            changes.push_back({FileTreeChange::Kind::removed, old_entry});
            forget_below(join(*key, old_entry.name));
        } else if (*same_name->second != old_entry) {
            changes.push_back({FileTreeChange::Kind::changed, *same_name->second});
            forget_below(join(*key, old_entry.name));
        }
    }
    for (const FileTreeEntry& new_entry : fresh.entries) {
        if (!before_names.contains(new_entry.name)) {
            changes.push_back({FileTreeChange::Kind::added, new_entry});
        }
    }
    sort_in_order(
        changes, [](const FileTreeChange& change) -> const FileTreeEntry& { return change.entry; });
    loaded_.insert_or_assign(*key, std::move(fresh));
    return changes;
}

} // namespace anvil::core
