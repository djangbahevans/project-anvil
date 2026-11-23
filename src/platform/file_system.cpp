#include "anvil/platform/file_system.hpp"

#include "file_times.hpp"

#include <algorithm>
#include <system_error>

namespace anvil::platform {
namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
#ifdef _WIN32
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.string();
#endif
}

Error failure(ErrorCode code, std::string_view verb, const fs::path& path,
              const std::string& detail) {
    return Error{code, "cannot " + std::string(verb) + " " + utf8(path) + ": " + detail};
}

Error failure(std::string_view verb, const fs::path& path, const std::error_code& error) {
    ErrorCode code = ErrorCode::io_error;
    if (error == std::errc::no_such_file_or_directory) {
        code = ErrorCode::not_found;
    } else if (error == std::errc::file_exists) {
        code = ErrorCode::already_exists;
    } else if (error == std::errc::not_a_directory) {
        code = ErrorCode::not_a_directory;
    } else if (error == std::errc::is_a_directory) {
        code = ErrorCode::is_a_directory;
    } else if (error == std::errc::permission_denied ||
               error == std::errc::operation_not_permitted) {
        code = ErrorCode::permission_denied;
    }
    return failure(code, verb, path, error.message());
}

struct Classification {
    unsigned type = 0;
    bool link = false;
    bool broken = false;
    fs::file_status target;
};

// What `path` is, without a failure: a path that does not exist has type 0.
Classification classify(const fs::path& path) {
    Classification result;
    std::error_code ignored;
    const fs::file_status own = fs::symlink_status(path, ignored);
    if (!fs::exists(own)) {
        return result;
    }
    result.link = fs::is_symlink(own);
    result.target = own;
    if (result.link) {
        result.target = fs::status(path, ignored);
        result.broken = !fs::exists(result.target);
    }
    if (!result.broken) {
        if (fs::is_regular_file(result.target)) {
            result.type |= file_type::file;
        } else if (fs::is_directory(result.target)) {
            result.type |= file_type::directory;
        }
    }
    if (result.link) {
        result.type |= file_type::symbolic_link;
    }
    return result;
}

bool is_inside(const fs::path& path, const fs::path& directory) {
    const fs::path relative = fs::absolute(path).lexically_normal().lexically_relative(
        fs::absolute(directory).lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}

bool is_same_path(const fs::path& a, const fs::path& b) {
    return fs::absolute(a).lexically_normal() == fs::absolute(b).lexically_normal();
}

} // namespace

std::expected<FileStat, Error> stat_path(const fs::path& path) {
    std::error_code error;
    const fs::file_status own = fs::symlink_status(path, error);
    if (!fs::exists(own)) {
        return std::unexpected(
            error ? failure("stat", path, error)
                  : failure(ErrorCode::not_found, "stat", path, "no such file or directory"));
    }
    const Classification classification = classify(path);
    FileStat result;
    result.type = classification.type;
    if (!classification.broken) {
        result.read_only =
            (classification.target.permissions() & fs::perms::owner_write) == fs::perms::none;
        if (fs::is_regular_file(classification.target)) {
            result.size = fs::file_size(path, error);
            if (error) {
                return std::unexpected(failure("stat", path, error));
            }
        }
    }
    const auto times = file_times(path, !classification.broken);
    if (!times) {
        return std::unexpected(
            failure(ErrorCode::io_error, "stat", path, "the times cannot be read"));
    }
    result.modified_ms = times->modified_ms;
    result.created_ms = times->created_ms.value_or(times->modified_ms);
    return result;
}

std::expected<std::vector<DirectoryEntry>, Error> list_directory(const fs::path& path) {
    std::error_code error;
    fs::directory_iterator iterator(path, error);
    if (error) {
        return std::unexpected(failure("list", path, error));
    }
    std::vector<DirectoryEntry> entries;
    for (; iterator != fs::directory_iterator(); iterator.increment(error)) {
        if (error) {
            return std::unexpected(failure("list", path, error));
        }
        entries.push_back({utf8(iterator->path().filename()), classify(iterator->path()).type});
    }
    if (error) {
        return std::unexpected(failure("list", path, error));
    }
    std::ranges::sort(entries, {}, &DirectoryEntry::name);
    return entries;
}

std::expected<void, Error> make_directory(const fs::path& path) {
    std::error_code error;
    const fs::file_status status = fs::status(path, error);
    if (fs::exists(status)) {
        if (fs::is_directory(status)) {
            return {};
        }
        return std::unexpected(failure(ErrorCode::already_exists, "create the directory", path,
                                       "a file exists there"));
    }
    fs::create_directories(path, error);
    if (error) {
        return std::unexpected(failure("create the directory", path, error));
    }
    return {};
}

std::expected<void, Error> remove_path(const fs::path& path, bool recursive) {
    std::error_code error;
    const fs::file_status own = fs::symlink_status(path, error);
    if (!fs::exists(own)) {
        return std::unexpected(
            failure(ErrorCode::not_found, "remove", path, "no such file or directory"));
    }
    if (fs::is_directory(own) && recursive) {
        fs::remove_all(path, error);
    } else {
        fs::remove(path, error);
    }
    if (error) {
        return std::unexpected(failure("remove", path, error));
    }
    return {};
}

std::expected<void, Error> rename_path(const fs::path& from, const fs::path& to, bool overwrite) {
    std::error_code error;
    if (!fs::exists(fs::symlink_status(from, error))) {
        return std::unexpected(
            failure(ErrorCode::not_found, "move", from, "no such file or directory"));
    }
    if (is_same_path(from, to)) {
        return {};
    }
    const bool target_exists = fs::exists(fs::symlink_status(to, error));
    if (target_exists && !overwrite) {
        return std::unexpected(failure(ErrorCode::already_exists, "move to", to, "it exists"));
    }
    if (target_exists && is_inside(from, to)) {
        return std::unexpected(
            failure(ErrorCode::invalid_argument, "move to", to, "the target contains the source"));
    }
    fs::rename(from, to, error);
    if (error && target_exists) {
        // A rename replaces a file by a file and an empty directory by a directory; anything else
        // needs the target out of the way.
        std::error_code removed;
        fs::remove_all(to, removed);
        if (removed) {
            return std::unexpected(failure("replace", to, removed));
        }
        fs::rename(from, to, error);
    }
    if (error) {
        return std::unexpected(failure("move", from, error));
    }
    return {};
}

std::expected<void, Error> copy_path(const fs::path& from, const fs::path& to, bool overwrite) {
    std::error_code error;
    const fs::file_status source = fs::symlink_status(from, error);
    if (!fs::exists(source)) {
        return std::unexpected(
            failure(ErrorCode::not_found, "copy", from, "no such file or directory"));
    }
    if (fs::is_directory(source) && (is_inside(to, from) || is_same_path(to, from))) {
        return std::unexpected(failure(ErrorCode::invalid_argument, "copy", from,
                                       "a directory cannot be copied into itself"));
    }
    if (fs::exists(fs::symlink_status(to, error))) {
        if (!overwrite) {
            return std::unexpected(failure(ErrorCode::already_exists, "copy to", to, "it exists"));
        }
        if (is_inside(from, to) || is_same_path(from, to)) {
            return std::unexpected(failure(ErrorCode::invalid_argument, "copy to", to,
                                           "the target contains the source"));
        }
        fs::remove_all(to, error);
        if (error) {
            return std::unexpected(failure("replace", to, error));
        }
    }
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, error);
    if (error) {
        return std::unexpected(failure("copy", from, error));
    }
    return {};
}

} // namespace anvil::platform
