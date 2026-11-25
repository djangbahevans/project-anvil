#include "anvil/exthost/workspace_file_edit.hpp"

#include "anvil/exthost/vscode_types.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/platform/file_system.hpp"

namespace anvil::exthost {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

Error failure(ErrorCode code, const std::string& message, const fs::path& path) {
    return Error{code, message + ": " + path.string()};
}

bool is_directory(unsigned type) {
    return (type & platform::file_type::directory) != 0;
}

std::expected<std::string, Error> decode_base64(std::string_view encoded) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r') {
            continue;
        }
        const std::size_t value = alphabet.find(c);
        if (value == std::string_view::npos) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "invalid base64 content"});
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    return decoded;
}

// The outermost directory in the chain of `directory` and its parents that does not exist.
std::optional<fs::path> outermost_missing(fs::path directory) {
    std::optional<fs::path> missing;
    while (!directory.empty() && directory != directory.parent_path()) {
        const auto stat = platform::stat_path(directory);
        if (stat) {
            break;
        }
        missing = directory;
        directory = directory.parent_path();
    }
    return missing;
}

void remove_quietly(const fs::path& path) {
    (void)platform::remove_path(path, true);
}

} // namespace

bool is_path_within(const fs::path& path, const fs::path& root) {
    const fs::path relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

fs::path path_moved(const fs::path& path, const fs::path& from, const fs::path& to) {
    const fs::path relative = path.lexically_relative(from);
    return relative == "." ? to : to / relative;
}

std::expected<FileOperation, Error> parse_file_operation(const json& item) {
    const auto resource = [&](const char* key) -> std::expected<std::optional<fs::path>, Error> {
        if (!item.contains(key) || item[key].is_null()) {
            return std::optional<fs::path>();
        }
        auto uri = uri_from_json(item[key]);
        if (!uri) {
            return std::unexpected(uri.error());
        }
        auto path = uri->file_path();
        if (!path) {
            return std::unexpected(Error{
                ErrorCode::unsupported, "only file: resources are supported: " + uri->to_string()});
        }
        return std::optional<fs::path>(std::move(*path));
    };
    auto old_path = resource("oldResource");
    if (!old_path) {
        return std::unexpected(old_path.error());
    }
    auto new_path = resource("newResource");
    if (!new_path) {
        return std::unexpected(new_path.error());
    }
    if (!old_path->has_value() && !new_path->has_value()) {
        return std::unexpected(Error{ErrorCode::unsupported, "an edit without a resource"});
    }

    FileOperation operation;
    if (old_path->has_value() && new_path->has_value()) {
        operation.kind = FileOperation::Kind::rename;
        operation.path = **old_path;
        operation.target = **new_path;
    } else if (old_path->has_value()) {
        operation.kind = FileOperation::Kind::remove;
        operation.path = **old_path;
    } else {
        operation.kind = FileOperation::Kind::create;
        operation.path = **new_path;
    }

    if (item.contains("options") && item["options"].is_object()) {
        const json& options = item["options"];
        if (options.value("copy", false)) {
            return std::unexpected(Error{ErrorCode::unsupported, "copying is not supported"});
        }
        if (options.contains("overwrite") && options["overwrite"].is_boolean()) {
            operation.overwrite = options["overwrite"].get<bool>();
        }
        operation.ignore_if_exists = options.value("ignoreIfExists", false);
        operation.ignore_if_not_exists = options.value("ignoreIfNotExists", false);
        operation.recursive = options.value("recursive", false);
        operation.folder = options.value("folder", false);
        if (options.contains("contents") && options["contents"].is_object()) {
            const json& contents = options["contents"];
            if (contents.value("type", "") != "base64" || !contents.contains("value") ||
                !contents["value"].is_string()) {
                return std::unexpected(
                    Error{ErrorCode::unsupported, "only base64 contents are supported"});
            }
            auto decoded = decode_base64(contents["value"].get<std::string>());
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            operation.contents = std::move(*decoded);
        }
    }
    return operation;
}

std::expected<std::optional<unsigned>, Error> FileOperationPlanner::type_of(fs::path path) const {
    for (auto effect = effects_.rbegin(); effect != effects_.rend(); ++effect) {
        switch (effect->kind) {
        case Effect::Kind::created_file:
            if (path == effect->path) {
                return std::optional<unsigned>(platform::file_type::file);
            }
            if (is_path_within(path, effect->path)) {
                return std::optional<unsigned>();
            }
            break;
        case Effect::Kind::created_directory:
            if (path == effect->path) {
                return std::optional<unsigned>(platform::file_type::directory);
            }
            if (is_path_within(path, effect->path)) {
                return std::optional<unsigned>();
            }
            break;
        case Effect::Kind::removed:
            if (is_path_within(path, effect->path)) {
                return std::optional<unsigned>();
            }
            break;
        case Effect::Kind::moved:
            if (is_path_within(path, effect->target)) {
                path = path_moved(path, effect->target, effect->path);
            } else if (is_path_within(path, effect->path)) {
                return std::optional<unsigned>();
            }
            break;
        }
    }
    const auto stat = platform::stat_path(path);
    if (stat) {
        return std::optional<unsigned>(stat->type);
    }
    if (stat.error().code == ErrorCode::not_found ||
        stat.error().code == ErrorCode::not_a_directory) {
        return std::optional<unsigned>();
    }
    return std::unexpected(stat.error());
}

std::expected<void, Error> FileOperationPlanner::check_parents(const fs::path& path) const {
    for (fs::path parent = path.parent_path(); !parent.empty() && parent != parent.parent_path();
         parent = parent.parent_path()) {
        const auto type = type_of(parent);
        if (!type) {
            return std::unexpected(type.error());
        }
        if (type->has_value()) {
            return is_directory(**type) ? std::expected<void, Error>()
                                        : std::unexpected(failure(ErrorCode::not_a_directory,
                                                                  "not a directory", parent));
        }
    }
    return {};
}

std::expected<bool, Error> FileOperationPlanner::check(const FileOperation& operation) {
    using Kind = FileOperation::Kind;
    const fs::path& path = operation.path;
    const auto existing = type_of(path);
    if (!existing) {
        return std::unexpected(existing.error());
    }

    switch (operation.kind) {
    case Kind::create:
        if (existing->has_value()) {
            if (!operation.overwrite && operation.ignore_if_exists) {
                return false;
            }
            if (!operation.overwrite.value_or(false)) {
                return std::unexpected(failure(ErrorCode::already_exists, "already exists", path));
            }
            if (is_directory(**existing)) {
                if (operation.folder) {
                    return false;
                }
                return std::unexpected(failure(ErrorCode::is_a_directory, "is a directory", path));
            }
        } else if (auto parents = check_parents(path); !parents) {
            return std::unexpected(parents.error());
        }
        effects_.push_back(
            {operation.folder ? Effect::Kind::created_directory : Effect::Kind::created_file,
             path,
             {}});
        return true;

    case Kind::remove:
        if (!existing->has_value()) {
            if (operation.ignore_if_not_exists) {
                return false;
            }
            return std::unexpected(failure(ErrorCode::not_found, "does not exist", path));
        }
        if (is_directory(**existing) && !operation.recursive) {
            const auto entries = platform::list_directory(path);
            if (entries && !entries->empty()) {
                return std::unexpected(
                    failure(ErrorCode::io_error, "the directory is not empty", path));
            }
        }
        effects_.push_back({Effect::Kind::removed, path, {}});
        return true;

    case Kind::rename: {
        const fs::path& target = operation.target;
        if (path == target) {
            return false;
        }
        if (!existing->has_value()) {
            return std::unexpected(failure(ErrorCode::not_found, "does not exist", path));
        }
        if (is_path_within(target, path) || is_path_within(path, target)) {
            return std::unexpected(failure(ErrorCode::invalid_argument,
                                           "cannot move to or over its own parent or content",
                                           target));
        }
        const auto destination = type_of(target);
        if (!destination) {
            return std::unexpected(destination.error());
        }
        if (destination->has_value()) {
            if (!operation.overwrite && operation.ignore_if_exists) {
                return false;
            }
            if (!operation.overwrite.value_or(false)) {
                return std::unexpected(
                    failure(ErrorCode::already_exists, "already exists", target));
            }
        } else if (auto parents = check_parents(target); !parents) {
            return std::unexpected(parents.error());
        }
        effects_.push_back({Effect::Kind::moved, path, target});
        return true;
    }
    }
    return false;
}

std::expected<PerformedOperation, Error> perform(const FileOperation& operation) {
    using Kind = FileOperation::Kind;
    FileOperationPlanner planner;
    const auto runs = planner.check(operation);
    if (!runs) {
        return std::unexpected(runs.error());
    }
    if (!*runs) {
        return PerformedOperation{};
    }
    const fs::path& path = operation.path;

    switch (operation.kind) {
    case Kind::create: {
        std::optional<std::string> replaced;
        std::optional<fs::path> created = outermost_missing(path.parent_path());
        if (const auto stat = platform::stat_path(path)) {
            auto previous = platform::read_file(path);
            if (!previous) {
                return std::unexpected(previous.error());
            }
            replaced = std::move(*previous);
        } else if (!created) {
            created = path;
        }
        auto undo = [path, replaced, created] {
            if (replaced) {
                (void)platform::write_file(path, *replaced);
            } else if (created) {
                remove_quietly(*created);
            }
        };
        auto made = platform::make_directory(path.parent_path());
        if (made) {
            made = operation.folder ? platform::make_directory(path)
                                    : platform::write_file(path, operation.contents);
        }
        if (!made) {
            undo();
            return std::unexpected(made.error());
        }
        return PerformedOperation{true, std::move(undo)};
    }

    case Kind::remove: {
        if (auto removed = platform::remove_path(path, operation.recursive); !removed) {
            return std::unexpected(removed.error());
        }
        return PerformedOperation{true, {}};
    }

    case Kind::rename: {
        const std::optional<fs::path> created = outermost_missing(operation.target.parent_path());
        if (auto made = platform::make_directory(operation.target.parent_path()); !made) {
            return std::unexpected(made.error());
        }
        if (auto renamed =
                platform::rename_path(path, operation.target, operation.overwrite.value_or(false));
            !renamed) {
            if (created) {
                remove_quietly(*created);
            }
            return std::unexpected(renamed.error());
        }
        return PerformedOperation{true, [from = path, to = operation.target, created] {
                                      (void)platform::rename_path(to, from, false);
                                      if (created) {
                                          remove_quietly(*created);
                                      }
                                  }};
    }
    }
    return PerformedOperation{};
}

} // namespace anvil::exthost
